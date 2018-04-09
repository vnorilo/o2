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
#include <tuple>
#include <future>

namespace o2 {
	std::string get_machine_identifier();

	struct symbol_t {
		std::string symbol;
	};

	struct void_t {};

	using int32 = std::int32_t;
	using int64 = std::int64_t;

	namespace detail {
#define FOREACH_PRIMITIVE \
	F(O2_FLOAT, float, f32) \
	F(O2_INT32, int32, i32) \
	F(O2_INT64, int64, i64) \
	F(O2_DOUBLE, double, f64) \
	F(O2_STRING, const char*, s) \
	F(O2_STRING, std::string, s) \
	F(O2_SYMBOL, symbol_t, S)

#define F(O2_TYPE, C_TYPE, UNION_MEMBER) \
static o2_type o2_signature(C_TYPE) { return O2_TYPE; }
FOREACH_PRIMITIVE
#undef F

		template <typename T> static T extract(o2_arg_ptr p);

#define F(O2_TYPE, C_TYPE, UNION_MEMBER) \
template <> static C_TYPE extract<C_TYPE>(o2_arg_ptr p) { return {p->UNION_MEMBER}; }
FOREACH_PRIMITIVE
#undef F

		template <typename T> static T o2_decode() { 
			auto p = o2_get_next(o2_signature(T()));
			if (!p) throw std::runtime_error("Type mismatch in method handler");
			return extract<T>(p); 
		}

		template <typename... TElem> struct static_list {
		};

		template <typename TFirst, typename... TRest> struct static_list<TFirst, TRest...> {
			using first_t = TFirst;
			using rest_t = static_list<TRest...>;
			rest_t rest() const { return rest_t{}; }
		};

		using static_nil = static_list<>;

		template <typename...> struct functor_traits;

		template <typename TRet, typename... TArgs> struct functor_traits<TRet, TArgs...> {
			using ret_t = TRet;
			using arg_t = std::tuple<TArgs...>;
			constexpr static bool is_query = true;

			constexpr const char* typestring() const {
				static const char sig[] = {
					'h', 's', (char)o2_signature(TArgs())...
				};
				return sig;
			}

			template <typename T, typename... TDecoded>
			auto _relay(const T& fn, static_nil, TDecoded&&... args) const {
				return fn(std::forward<TDecoded>(args)...);
			}

			template <typename T, typename TList, typename... TDecoded> 
			auto _relay(const T& fn, TList to_decode, TDecoded&&... args) const {
				return _relay(fn, to_decode.rest(), o2_decode<typename TList::first_t>(), std::forward<TDecoded>(args)...);
			}

			template <typename T> auto relay(const T& fn) const {
				return _relay(fn, static_list<TArgs...>{});
			}
		};

		template <typename... TArgs> struct functor_traits<void, TArgs...> {
			using ret_t = void;
			using arg_t = std::tuple<TArgs...>;
			constexpr static bool is_query = false;

			constexpr const char* typestring() const {
				static const char sig[] = {
					(char)o2_signature(TArgs())...
				};
				return sig;
			}

			template <typename T, typename... TDecoded>
			void _relay(const T& fn, static_nil, TDecoded&&... args) const {
				fn(std::forward<TDecoded>(args)...);
			}

			template <typename T, typename TList, typename... TDecoded>
			void _relay(const T& fn, TList to_decode, TDecoded&&... args) const {
				_relay(fn, to_decode.rest(), o2_decode<typename TList::first_t>(), std::forward<TDecoded>(args)...);
			}

			template <typename T> void_t relay(const T& fn) const {
				_relay(fn, static_list<TArgs...>{});
				return {};
			}
		};

		template <typename TObj, typename TRet, typename... TArgs>
		functor_traits<TRet, TArgs...> get_traits(const TObj&, TRet(TObj::*)(TArgs...) const) {
			return {};
		}

		template <typename T> auto infer_signature(const T& fn, void(T::*class_sfinae)() = nullptr) {
			return get_traits(fn, &T::operator());
		}

		template <typename TRet, typename... TArgs> functor_traits<TRet, TArgs...> infer_signature(TRet(*)(TArgs...)) {
			return {};
		}
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

#undef FOREACH_PRIMITIVE
#define FOREACH_PRIMITIVE F(float) F(int32) F(double) F(int64) F(bool) F(char)
#define F(Type) static void add(Type v) { o2_add_ ## Type (v); }
		FOREACH_PRIMITIVE
#undef FOREACH_PRIMITIVE

		// SFINAE for containers that have a ::data() member that returns a pointer
		template <typename T> static void add(const T& container, decltype(std::declval<T>().data()) = nullptr) {
			o2_add_vector(detail::o2_signature(*container.data()), (int)container.size(), (void*)container.data());
		}

		// encode a C array
		template <typename TElement, size_t TSize> static void add(const TElement (&data) [TSize]) {
			o2_add_vector(detail::o2_signature(data[0]), TSize, (void*)data);
		}

		template <size_t TSize> static void add(const char(&data)[TSize]) {
			o2_add_string(data);
		}

		static void add(const std::string& str) {
			o2_add_string(str.c_str());
		}

		static void add(const char* str) {
			o2_add_string(str);
		}

		static void add(const symbol_t& sym) {
			o2_add_string(sym.symbol.c_str());
		}

		static void add(void_t) {}

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
			auto address = on_reply(app, id, std::move(reply_handler));
			o2_send_start();
			encode(id++, address, args...);
			o2_send_finish(0.0, path.c_str(), 1);
		}

		template <typename T> struct query_builder;

		template <typename TRet, typename... TArgs>
		struct query_builder<TRet(TArgs...)> {
			std::function<std::future<TRet>(TArgs...)> get_fn(const client& c, std::string method) const {
				return std::move([&c, method = std::move(method)](auto... args) {
					auto prom = std::make_shared<std::promise<TRet>>();
					auto fut = prom->get_future();
					c.query(method.c_str(), [&c, prom](const char *ty) mutable {
						try {
							prom->set_value(detail::o2_decode<TRet>());
						} catch (...) {
							prom->set_exception(std::current_exception());
						}
					}, std::forward<decltype(args)>(args)...);
					return fut;
				});
			}
		};

		template <typename... TArgs>
		struct query_builder<void(TArgs...)> {
			std::function<void(TArgs...)> get_fn(const client& c, std::string method) const {
				return[&c, method = std::move(method)](auto... args) {
					c.send(method.c_str(), std::forward<decltype(args)>(args)...);
				};
			}
		};

		template <typename T> auto proxy(std::string method) const {
			return query_builder<T>().get_fn(*this, std::move(method));
		}
	};

	class service {
		friend class application;

		const application &app;
		std::string name;

		struct method_record_t {
			method_t proc;
		};

		using wrapper_t = std::function<void(const o2_msg_data_ptr msg, const char *ty)>;

		std::forward_list<std::unique_ptr<method_record_t>> methods;
		std::forward_list<std::unique_ptr<wrapper_t>> wrappers;

		static void callback_wrapper(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void *user);
		static void callback_relay(const o2_msg_data_ptr msg, const char* ty, o2_arg_ptr* argv, int argc, void *user);

		service(const application& a, std::string n);

	public:
		service(const service&) = delete;
		~service();
		service(service&& from);
		service& operator=(service&& s);

		int implement(std::string path, const std::string& ty, method_t method);
		int implement(std::string path, const std::string& ty, std::string doc, method_t method);

		template <typename TFnObj>
		int implement(std::string path, const TFnObj& fn) {
			auto traits = detail::infer_signature(fn);
			wrappers.emplace_front(std::make_unique<wrapper_t>([fn, traits](const o2_msg_data_ptr msg, const char *ty) {
				if (traits.is_query) {
					auto id = detail::o2_decode<int64>();
					auto reply = detail::o2_decode<std::string>();
					o2_send_start();
					o2_add_int64(id);
					client::add(traits.relay(fn));
					reply += "/get-reply";
					o2_send_finish(0.0, reply.c_str(), 1);
				} else {
					traits.relay(fn);
				}
			}));
			path = "/" + name + "/" + path;
			return o2_method_new(path.c_str(), traits.typestring(), callback_relay, wrappers.front().get(), false, false);
		}

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

		std::string get_reply_address() const {
			return "!" + local_process;
		}
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