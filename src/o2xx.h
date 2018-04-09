#pragma once

#include "o2.h"

#include <cstdint>
#include <functional>
#include <forward_list>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace o2 {
	std::string get_machine_identifier();

	struct symbol_t {
		std::string symbol;
	};

	using int32 = std::int32_t;
	using int64 = std::int64_t;

	namespace detail {
		static o2_type o2_signature(float) { return O2_FLOAT; }
		static o2_type o2_signature(int32) { return O2_INT32; }
		static o2_type o2_signature(int64) { return O2_INT64; }
		static o2_type o2_signature(double) { return O2_DOUBLE; }
		static o2_type o2_signature(const char*) { return O2_STRING; }
		static o2_type o2_signature(std::string) { return O2_STRING; }
		static o2_type o2_signature(symbol_t) { return O2_SYMBOL; }
	}

	using method_t = std::function<void(o2_arg_ptr*, int)>;	
	using reply_handler_t = std::function<void(const char*)>;

	class application;
	std::string on_reply(const application&, std::int64_t id, reply_handler_t);
	extern std::recursive_mutex msg_lock;

	class client {
		friend class application;
		friend class service;
		std::string name;
		const application& app;

		client(const application& a, std::string n):name(std::move(n)), app(a) { }

#define FOREACH_PRIMITIVE F(float) F(int32) F(double) F(int64) F(bool) F(char)
#define F(Type) void add(Type v) const { o2_add_ ## Type (v); }
		FOREACH_PRIMITIVE
#undef FOREACH_PRIMITIVE

		// SFINAE for containers that have a ::data() member that returns a pointer
		template <typename T> void add(const T& container, decltype(std::declval<T>().data()) = nullptr) const {
			o2_add_vector(detail::o2_signature(*container.data()), (int)container.size(), (void*)container.data());
		}

		// encode a C array
		template <typename TElement, size_t TSize> void add(const TElement (&data) [TSize]) const {
			o2_add_vector(detail::o2_signature(data[0]), TSize, (void*)data);
		}

		template <size_t TSize> void add(const char(&data)[TSize]) const {
			o2_add_string(data);
		}

		void add(const std::string& str) const {
			o2_add_string(str.c_str());
		}

		void add(const symbol_t& sym) const {
			o2_add_string(sym.symbol.c_str());
		}

		void encode() const {}

		template <typename TArg, typename... TArgs> void encode(const TArg& a, const TArgs&... as) const {
			add(a); encode(as...);
		}

	public:
		template <typename... TArgs> void send(o2_time time, const char* method, TArgs&&... args) const {
			std::string path = "!" + name + "/" + method;
			std::lock_guard<std::recursive_mutex> lg(msg_lock);
			o2_send_start();
			encode(args...);
			o2_send_finish(time, path.c_str(), 1);
		}

		template <typename... TArgs> void send(const char* method, TArgs&&... args) const {
			send(0.0, method, std::forward<TArgs>(args)...);
		}

		template <typename... TArgs>
		void query(const char* method, reply_handler_t reply_handler, TArgs&&... args) const {
			std::string path = "!" + name + "/" + method;
			std::lock_guard<std::recursive_mutex> lg(msg_lock);
			static std::int64_t id = 0;
			auto address = on_reply(app, id, reply_handler);
			o2_send_start();
			encode(id++, address, args...);
			o2_send_finish(0.0, path.c_str(), 1);
		}
	};

	class service {
		friend class application;

		const application &app;
		std::string name;

		struct method_record_t {
			method_t proc;
		};

		std::forward_list<std::unique_ptr<method_record_t>> methods;

		static void callback_wrapper(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void *user);

		service(const application& a, std::string n);

	public:
		service(const service&) = delete;
		~service();
		service(service&& from);
		service& operator=(service&& s);

		int implement(std::string path, const std::string& ty, method_t method);
		int implement(std::string path, const std::string& ty, std::string doc, method_t method);

		const std::string& get_name() const {
			return name;
		}
	};
	class application {
		std::thread worker;
		mutable std::unordered_map<std::int64_t, reply_handler_t> reply_handlers;
		static void reply_wrapper(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void* user);
		friend std::string on_reply(const application&, std::int64_t, reply_handler_t);
		std::string local_process;
	public:
		const std::string name;

		application(std::string n, int rate = 100);
		~application();
		application(const application&) = delete;

		service provide(std::string n) const;
		client request(std::string n) const;

	};

	class directory {
		struct metadata_t {
			const char* doc;
			const char* typestring;
		};
		const char* unique(const char *str);
		std::unordered_set<std::string> string_pool;
		std::unordered_map<std::string, metadata_t> metadata;
		service svc;
	public:
		directory(const application&);

		using enumerate_callback_t = std::function<void(const char*,const char*,const char*)>;
		void enumerate(std::string search_pattern, enumerate_callback_t) const;
	};
}