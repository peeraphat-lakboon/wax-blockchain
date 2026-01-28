#pragma once

#define API_CALL(call_name, api_handle, api_namespace, params_type)                                                                         \
    [&api_handle](std::string&& body, ws_response_callback&& cb) mutable {                                                                  \
        try {                                                                                                                               \
            auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                                             \
            auto result = api_handle.call_name(std::move(params), api_handle.start());                                                      \
            cb( fc::variant(result) );                                                                                                      \
        } catch (...) {                                                                                                                     \
            websocket_rpc_plugin::handle_exception( cb );                                                                                   \
        }                                                                                                                                   \
    }

#define API_CALL_POST(call_name, api_handle, api_namespace, params_type, result_type)                                                       \
    [&api_handle](std::string&& body, ws_response_callback&& cb) mutable {                                                                  \
        try {                                                                                                                               \
            auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                                             \
            using task_t = std::function<chain::t_or_exception<result_type>()>;                                                             \
            task_t task = api_handle.call_name(std::move(params), api_handle.start());                                                      \
            app().get_plugin<websocket_rpc_plugin>().post_thread_pool(                                                                      \
                [cb=std::move(cb), task = std::move(task)]() {                                                                              \
                try {                                                                                                                       \
                    auto result = task();                                                                                                   \
                    if (std::holds_alternative<fc::exception_ptr>(result)) {                                                                \
                        throw *std::get<fc::exception_ptr>(result);                                                                         \
                    } else {                                                                                                                \
                        cb( fc::variant(std::get<result_type>(std::move(result))) );                                                        \
                    }                                                                                                                       \
                } catch (...) {                                                                                                             \
                    websocket_rpc_plugin::handle_exception( cb );                                                                           \
                }                                                                                                                           \
            });                                                                                                                             \
        } catch (...) {                                                                                                                     \
            websocket_rpc_plugin::handle_exception( cb );                                                                                   \
        }                                                                                                                                   \
    }

#define API_CALL_ASYNC(call_name, api_handle, api_namespace, params_type, result_type)                                                      \
    [&api_handle](string&& body, ws_response_callback&& cb) mutable {                                                                       \
        api_handle.start();                                                                                                                 \
        try {                                                                                                                               \
            auto params = parse_params<api_namespace::call_name ## _params, params_type>(body);                                             \
            using task_t = std::function<chain::t_or_exception<result_type>()>;                                                             \
            api_handle.call_name( std::move(params),                                                                                        \
                [cb=std::move(cb), body=std::move(body)]                                                                                    \
                (const chain::next_function_variant<result_type>& result) mutable {                                                         \
                    if (std::holds_alternative<fc::exception_ptr>(result)) {                                                                \
                        try {                                                                                                               \
                            throw *std::get<fc::exception_ptr>(result);                                                                     \
                        } catch (...) {                                                                                                     \
                            websocket_rpc_plugin::handle_exception( cb );                                                                   \
                        }                                                                                                                   \
                    } else if (std::holds_alternative<result_type>(result)) {                                                               \
                        cb( fc::variant(std::get<result_type>(result)) );                                                                   \
                    } else {                                                                                                                \
                        assert(std::holds_alternative<task_t>(result));                                                                     \
                        app().get_plugin<websocket_rpc_plugin>().post_thread_pool(                                                          \
                            [cb=std::move(cb), body=std::move(body), task = std::get<task_t>(std::move(result))]() {                        \
                            chain::t_or_exception<result_type> result = task();                                                             \
                            if (std::holds_alternative<fc::exception_ptr>(result)) {                                                        \
                                try {                                                                                                       \
                                    throw *std::get<fc::exception_ptr>(result);                                                             \
                                } catch (...) {                                                                                             \
                                    websocket_rpc_plugin::handle_exception( cb );                                                           \
                                }                                                                                                           \
                            } else {                                                                                                        \
                                cb( fc::variant(std::get<result_type>(std::move(result))) );                                                \
                            }                                                                                                               \
                        });                                                                                                                 \
                    }                                                                                                                       \
                });                                                                                                                         \
        } catch (...) {                                                                                                                     \
            websocket_rpc_plugin::handle_exception( cb );                                                                                   \
        }                                                                                                                                   \
    }

#define RO_CALL(call_name, api_handle, params_type) API_CALL(call_name, api_handle, chain_apis::read_only, params_type)
#define RW_CALL(call_name, api_handle, params_type) API_CALL(call_name, api_handle, chain_apis::read_write, params_type)
#define RO_CALL_POST(call_name, api_handle, result_type, params_type) API_CALL_POST(call_name, api_handle, chain_apis::read_only, params_type, result_type)
#define RO_CALL_ASYNC(call_name, api_handle, result_type, params_type) API_CALL_ASYNC(call_name, api_handle, chain_apis::read_only, params_type, result_type)
#define RW_CALL_ASYNC(call_name, api_handle, result_type, params_type) API_CALL_ASYNC(call_name, api_handle, chain_apis::read_write, params_type, result_type)