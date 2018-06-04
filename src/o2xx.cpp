#include "o2xx.h"

extern "C" {
#include "o2_internal.h"
}

#include <regex>

#ifndef NDEBUG
#include <iostream>
#endif

namespace o2 {
	std::recursive_mutex& msg_lock() {
		static std::recursive_mutex l;
		return l;
	}

	void service::callback_wrapper(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void *user) {
		auto method = (method_record_t*)user;
		method->proc(argv, argc);
	}

	void service::callback_relay(const o2_msg_data_ptr msg, const char *ty, o2_arg_ptr* argv, int argc, void* user) {
		auto relay = (wrapper_t*)user;
		o2_extract_start(msg);
		(*relay)(msg, ty);
	}

	service::service(const application& a, std::string n) :app(a), name(std::move(n)) {
		std::lock_guard<std::recursive_mutex> lg(msg_lock());
		auto err = o2_service_new(name.c_str());
		switch (err) {
			case O2_SERVICE_EXISTS:
				own_service = false;
			case O2_SUCCESS:
				return;
			default:
				throw std::runtime_error("failed to create o2 service");
		}
	}

	service::~service() {
		if (own_service) {
			std::lock_guard<std::recursive_mutex> lg(msg_lock());
			o2_service_free((char*)name.c_str());
			app.request("directory").send("remove-service", symbol_t{ name });
		}
	}


	service::service(service&& from) :app(from.app) {
		std::swap(name, from.name);
		std::swap(methods, from.methods);
		std::swap(own_service, from.own_service);
	}

	service& service::operator=(service&& s) {
		std::swap(name, s.name);
		std::swap(methods, s.methods);
		std::swap(own_service, s.own_service);
		return *this;
	}

	int service::implement(std::string path, const std::string& ty, method_t method) {
		methods.emplace_front(std::make_unique<method_record_t>(method_record_t{
			std::move(method)
		}));
		path = "/" + name + "/" + path;
		std::lock_guard<std::recursive_mutex> lg(msg_lock());
		return o2_method_new(path.c_str(), ty.c_str(), callback_wrapper, methods.front().get(), true, true);
	}

	int service::implement(std::string path, const std::string& ty, std::string doc, method_t method) {
		if (!implement(path, ty, method)) {
			app.request("directory").send("add-method", symbol_t{ name + "/" + path }, ty, doc);
			return O2_SUCCESS;
		} else return O2_FAIL;
	}

	void client::wait_for_discovery(int poll_rate) {
		while (o2_status(name.c_str()) == O2_FAIL) {
			std::this_thread::sleep_for(std::chrono::milliseconds(poll_rate));
		}
	}

	application::application(std::string n, int rate) :name(n) {
		if (o2_initialize(n.c_str()) == O2_FAIL) throw std::runtime_error("Could not initialize o2");

		local_process = o2_process->proc.name;

		o2_service_new(local_process.c_str());
		o2_method_new((get_reply_address() + "/get-reply").c_str(), nullptr, reply_wrapper, this, 0, 0);

		worker = std::thread([rate]() {
			auto sleep_dur = std::chrono::microseconds(1000000 / rate);
			while (!o2_stop_flag) {
				tick();
				std::this_thread::sleep_for(sleep_dur);
			}
		});
	}

	void application::reply_wrapper(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void* user) {
		auto app = (application*)user;
		o2_extract_start(msg);
		auto id = o2_get_next(O2_INT64);
		if (id) {
			auto h = app->reply_handlers.find(id->h);
			if (h != app->reply_handlers.end()) {
				h->second(ty + 1);
				app->reply_handlers.erase(h);
			}
		}
	}

	void application::tick() {
		std::lock_guard<std::recursive_mutex> lg(msg_lock());
		o2_poll();
	}

	application::~application() {
		o2_stop_flag = true;
        worker.join();
		o2_finish();
    }

	service application::provide(std::string n) const {
		return { *this, n };
	}

	client application::request(std::string n) const {
		return { *this, n };
	}

	std::string on_reply(const application& app, std::int64_t id, reply_handler_t handler) {
		app.reply_handlers.emplace(id, std::move(handler));
		return app.get_reply_address();
	}

	const char* directory::unique(const char *str) {
		auto f = string_pool.find(str);
		if (f == string_pool.end()) {
			return string_pool.emplace(str).first->c_str();
		} else {
			return f->c_str();
		}
	}

	directory::directory(const application& app):svc(app.provide("directory")) {
		svc.implement("regex", "hss", [this](o2_arg_ptr* argv, int argn) {
			auto reply_id = argv[0]->h;
			std::string reply_address = argv[1]->s;
			std::string search_pattern = argv[2]->s;
			std::lock_guard<std::recursive_mutex> lg(msg_lock());

			o2_send_start();
			o2_add_int64(reply_id);
			this->enumerate(search_pattern, [](const std::string& method, const std::string& typestring, const std::string& doc) {
				o2_add_symbol(method.c_str());
				o2_add_string(typestring.c_str());
				o2_add_string(doc.c_str());
			});

			reply_address += "/get-reply";
			o2_send_finish(0.0, reply_address.c_str(), 1);
		});

		svc.implement("add-method", "Sss", [this](o2_arg_ptr* argv, int argn) {
			this->metadata.emplace(argv[0]->S, metadata_t{ unique(argv[1]->s), unique(argv[2]->s) });
		});

		svc.implement("remove-method", "S", [this](o2_arg_ptr* argv, int argn) {
			this->metadata.erase(argv[0]->S);
		});

		svc.implement("remove-service", "S", [this](o2_arg_ptr* argv, int argn) {
			std::string prefix = argv[0]->S;
			for (auto i = this->metadata.begin(); i != this->metadata.end();) {
				if (i->first.find(prefix) == 0) {
					this->metadata.erase(i++);
				} else {
					i++;
				}
			}
		});
	}

	void directory::enumerate(std::string search_pattern, enumerate_callback_t cb) const {
		std::regex matcher{ search_pattern };
		for (auto &m : metadata) {
			if (std::regex_match(m.first, matcher)) {
				cb(m.first.c_str(), m.second.doc, m.second.typestring);
			}
		}
	}
}
