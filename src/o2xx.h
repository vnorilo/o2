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
	/// \brief c++ type that represents the O2_SYMBOL type
	struct symbol_t {
        symbol_t(const char *ptr):symbol(ptr) {}
        symbol_t(std::string str):symbol(std::move(str)) {}
		std::string symbol;
	};

	/// \brief 'void' that can be assigned to for generic code
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
		static inline o2_type o2_signature(C_TYPE) { return O2_TYPE; }
FOREACH_PRIMITIVE
#undef F

		template <typename T> static inline T extract(o2_arg_ptr p);

#define F(O2_TYPE, C_TYPE, UNION_MEMBER) \
template <> inline C_TYPE extract<C_TYPE>(o2_arg_ptr p) { return (std::decay<decltype(p->UNION_MEMBER)>::type)p->UNION_MEMBER; }
FOREACH_PRIMITIVE
#undef F

		/// \brief templated o2_get_next
		template <typename T> static T o2_decode() { 
			auto p = o2_get_next(o2_signature(T()));
			if (!p) throw std::runtime_error("Type mismatch in method handler");
			return extract<T>(p); 
		}

		/// \brief compile-time type list terminator for template metaprogramming
		template <typename... TElem> struct static_list { };

		/// \brief compile-time type list for template metaprogramming
		template <typename TFirst, typename... TRest> struct static_list<TFirst, TRest...> {
			using first_t = TFirst;
			using rest_t = static_list<TRest...>;
			rest_t rest() const { return rest_t(); }
		};

		template <typename TFnObj, typename... TDecoded>
		static auto handle_o2_message(const TFnObj& fn, static_list<>, TDecoded&&... args) {
			return fn(std::forward<TDecoded>(args)...);
		}

		/// \brief relay o2 message parameters to a callable c++ object
		template <typename TFnObj, typename TList, typename... TDecoded>
		static auto handle_o2_message(const TFnObj& fn, TList to_decode, TDecoded&&... args) {
			return handle_o2_message(fn, to_decode.rest(), std::forward<TDecoded>(args)..., o2_decode<typename TList::first_t>());
		}

		/// \brief derive typestring from method parameters and support o2 method 'return values'
		template <typename...> struct functor_traits;

		template <typename TRet, typename... TArgs> struct functor_traits<TRet, TArgs...> {
			using ret_t = TRet;
			using arg_t = std::tuple<TArgs...>;
			constexpr static bool is_query = true;

			static const char* typestring() {
                static const char sig[] = {
					'h', 's', (char)o2_signature(TArgs())..., '\0'
				};
				return sig;
			}

			template <typename T> auto relay(T fn) const {
				return handle_o2_message(fn, static_list<TArgs...>{});
			}
		};

		template <typename... TArgs> struct functor_traits<void, TArgs...> {
			using ret_t = void;
			using arg_t = std::tuple<TArgs...>;
			constexpr static bool is_query = false;

			static const char* typestring() {
				static const char sig[] = {
					(char)o2_signature(TArgs())..., '\0'
				};
				return sig;
			}

			template <typename T> void_t relay(const T& fn) const {
				handle_o2_message(fn, static_list<TArgs...>{});
				return {};
			}
		};

		/// \brief generate functor_traits from a functor object
		template <typename TObj, typename TRet, typename... TArgs>
		functor_traits<TRet, TArgs...> get_functor_traits(const TObj&, TRet(TObj::*)(TArgs...) const) {
			return {};
		}

		/// \brief functor_traits getter specialization for objects with member function pointers.
		///		   this is targeted at functor objects
		template <typename T> auto get_traits(const T& fn, void(T::*class_sfinae)() = nullptr) {
			return get_functor_traits(fn, &T::operator());
		}

		/// \brief functor_traits getter specialization for plain function pointers
		template <typename TRet, typename... TArgs> functor_traits<TRet, TArgs...> get_traits(TRet(*)(TArgs...)) {
			return {};
		}
	}

	/// \brief std::function definition for handling parsed and coerced o2 messages
	using method_t = std::function<void(o2_arg_ptr*, int)>;	

	class application;
	using reply_handler_t = std::function<void(const char*)>;

	/// \brief per-process reply handler to support o2 reply messages
	std::string on_reply(const application&, std::int64_t id, reply_handler_t);

	/// \brief because o2 is not re-entrant, provide a global lock for threaded access
	extern std::recursive_mutex msg_lock;

	/// \brief c++ class that represents a remote o2 service
	class client {
		friend class application;
		friend class service;
		std::string name;
		const application& app;

		/// \brief only application can construct clients to ensure o2 is properly initialized
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

		// specialize for char[] arrays to avoid sending literal strings as O2 arrays
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

		static void encode() {}

		/// \brief encode any number of parameters type-safely for sending
		template <typename TArg, typename... TArgs> static void encode(const TArg& a, const TArgs&... as) {
			add(a); encode(as...);
		}

	public:
		/// \brief send a timed o2 message
		template <typename... TArgs> void send(o2_time time, const char* method, TArgs&&... args) const {
			std::lock_guard<std::recursive_mutex> lg(msg_lock);
			o2_send_start();
			encode(args...);
			o2_send_finish(time, ("!" + name + "/" + method).c_str(), 1);
		}

		/// \brief send a o2 message immediately
		template <typename... TArgs> void send(const char* method, TArgs&&... args) const {
			send(0.0, method, std::forward<TArgs>(args)...);
		}

		/// \brief send a o2 query with a return value
		///
		/// A sender id and a reply-address will be generated and prepended
		/// to the message data. A reply-handler hook will be generated on the
		/// local process, and it will call 'reply_handler' when the reply
		/// arrives. There is no provision for time-out.
		static std::int64_t get_id() {
			static std::int64_t id = 1;
			return id++;
		}

		template <typename... TArgs>
		void query(const char* method, reply_handler_t reply_handler, TArgs&&... args) const {
			std::lock_guard<std::recursive_mutex> lg(msg_lock);
			auto id = get_id();
			auto address = on_reply(app, id, std::move(reply_handler));
			o2_send_start();
			encode(id++, address, args...);
			o2_send_finish(0.0, ("!" + name + "/" + method).c_str(), 1);
		}

		template <typename T> struct query_builder;

		/// \brief helper struct to wrap queries into async futures
		template <typename TRet, typename... TArgs>
		struct query_builder<TRet(TArgs...)> {
			std::function<std::future<TRet>(TArgs...)> get_fn(const client& c, std::string method) const {
				return [&c, method = std::move(method)](auto&&... args) {
					auto prom = std::make_shared<std::promise<TRet>>();
					auto fut = prom->get_future();
					c.query(method.c_str(), [=](const char *ty) mutable {
						try {
							prom->set_value(detail::o2_decode<TRet>());
						} catch (...) {
							prom->set_exception(std::current_exception());
						}
					}, std::forward<decltype(args)>(args)...);
					return fut;
				};
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

		/// \brief make a callable proxy object that represents a remote o2 method.
		/// 
		/// The signature of the remote function should be provided as the template
		/// parameter T similarly to std::function. For example, a method that does
		/// not return anything could have a signature of void(int, const char*).
		///
		/// If a return value 'TRet' is provided, the proxy object will return
		/// a std::future<TRet>.
		template <typename T> auto proxy(std::string method) const {
			return query_builder<T>().get_fn(*this, std::move(method));
		}
	};

	/// \brief represents an o2 service provided by the local process
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

		/// \brief create a o2 method with the supplied typestring and method handler
		int implement(std::string path, const std::string& ty, method_t method);
		
		/// \brief create a documented o2 method with the supplied typestring and method handler.
		///
		/// calling this method will cause the method to be registered with the o2 directory
		/// service. In addition to the user-provided documentation string, the method
		/// address and type will be stored in the directory.
		int implement(std::string path, const std::string& ty, std::string doc, method_t method);

		/// \brief create a o2 method, by inferring the typestring from the passed method handler.
		///
		/// if the method handler has a return value, provide the query parameters automatically.
		template <typename TFnObj>
		int implement(std::string path, const TFnObj& fn) {
			auto traits = detail::get_traits(fn);
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
			return o2_method_new(path.c_str(), traits.typestring(), callback_relay, wrappers.front().get(), true, false);
		}

		const std::string& get_name() const {
			return name;
		}
	};

	/// \brief singleton class that represents the current o2 application.
	///
	/// application provides instantiation of clients and services, and contains
	/// some of the generic reply handling implementation.
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

	/// \brief provides a directory service for the o2 methods in the application
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
