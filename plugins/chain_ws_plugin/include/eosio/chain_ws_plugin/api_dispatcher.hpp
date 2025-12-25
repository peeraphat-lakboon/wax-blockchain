#pragma once

#include <eosio/chain/types.hpp>
#include <eosio/chain/controller.hpp>

#include <fc/compress/zlib.hpp>

namespace eosio::chain_ws {
    class ws_api_dispatcher {
        public:
            ws_api_dispatcher(fc::microseconds max_response_time): max_response_time(max_response_time) {};

            void call(const ws_request_type& api_name, session_base* session, ws_header h, std::vector<char> payload) {
                ws_response resp{ .request_id = h.request_id, .type = h.type };

                try {
                    auto& chain = app().get_plugin<chain_plugin>();

                    auto ro_api = chain.get_read_only_api(max_response_time);
                    auto rw_api = chain.get_read_write_api(max_response_time);

                    auto ro_deadline = ro_api.start();
                    auto rw_deadline = rw_api.start(); 

                    fc::variant request_params = parse_json(payload);
                    fc::variant variant_response;

                    #define CALL_RO_API_NP( METHOD ) \
                        case ws_request_type::METHOD: { \
                            chain_apis::read_only::METHOD##_params params; \
                            auto result = ro_api.METHOD(params, ro_deadline); \
                            fc::to_variant(result, variant_response); \
                            break; \
                        }

                    #define CALL_RO_API_P( METHOD ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<chain_apis::read_only::METHOD##_params>(); \
                            auto result = ro_api.METHOD(params, ro_deadline); \
                            fc::to_variant(result, variant_response); \
                            break; \
                        }

                    #define CALL_RO_API_P_V( METHOD ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<chain_apis::read_only::METHOD##_params>(); \
                            variant_response = ro_api.METHOD(params, ro_deadline); \
                            break; \
                        }

                    #define CALL_RO_API_P_E( METHOD, RESULT_TYPE ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<eosio::chain_apis::read_only::METHOD##_params>(); \
                            auto fn = ro_api.METHOD(params, ro_deadline); \
                            auto result  = fn(); \
                            using result_t = eosio::chain_apis::read_only::RESULT_TYPE; \
                            if (std::holds_alternative<result_t>(result)) { \
                                fc::to_variant(std::get<result_t>(result), variant_response); \
                            } else { \
                                auto e = std::get<std::shared_ptr<fc::exception>>(result); \
                                resp.error = e->top_message(); \
                            } \
                            break; \
                        }

                    #define CALL_RO_API_P_E_V( METHOD ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<eosio::chain_apis::read_only::METHOD##_params>(); \
                            auto fn = ro_api.METHOD(params, ro_deadline); \
                            auto result  = fn(); \
                            if (std::holds_alternative<fc::variant>(result)) { \
                                variant_response = std::get<fc::variant>(result); \
                            } else { \
                                auto e = std::get<std::shared_ptr<fc::exception>>(result); \
                                resp.error = e->top_message(); \
                            } \
                            break; \
                        }

                    #define CALL_RO_API_ASYNC( METHOD, RESULT_TYPE ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<chain_apis::read_only::METHOD##_params>(); \
                            ro_api.METHOD(std::move(params), [this, session, h](const chain::next_function_variant<chain_apis::read_only::RESULT_TYPE>& result) mutable { \
                                ws_response resp{ .request_id = h.request_id, .type = h.type }; \
                                \
                                if (std::holds_alternative<fc::exception_ptr>(result)) { \
                                    resp.error = std::get<fc::exception_ptr>(result)->to_detail_string(); \
                                    send_response(session, std::move(resp), fc::variant()); \
                                } else if (std::holds_alternative<chain_apis::read_only::RESULT_TYPE>(result)) { \
                                    fc::variant variant_response; \
                                    fc::to_variant(std::get<chain_apis::read_only::RESULT_TYPE>(result), variant_response); \
                                    send_response(session, std::move(resp), std::move(variant_response)); \
                                } else { \
                                    using rw_fwd_t = std::function<chain::t_or_exception<chain_apis::read_only::RESULT_TYPE>()>; \
                                    auto& http_plugin_pool = app().get_plugin<http_plugin>(); \
                                    \
                                    http_plugin_pool.post_http_thread_pool([this, session, resp = std::move(resp), fwd = std::get<rw_fwd_t>(std::move(result))]() mutable { \
                                        auto final_result = fwd(); \
                                        if (std::holds_alternative<fc::exception_ptr>(final_result)) { \
                                            resp.error = std::get<fc::exception_ptr>(final_result)->to_detail_string(); \
                                            send_response(session, std::move(resp), fc::variant()); \
                                        } else { \
                                            fc::variant variant_response; \
                                            fc::to_variant(std::get<chain_apis::read_only::RESULT_TYPE>(final_result), variant_response); \
                                            send_response(session, std::move(resp), std::move(variant_response)); \
                                        } \
                                    }); \
                                } \
                            }); \
                            return;\
                        }

                    #define CALL_RW_API_ASYNC( METHOD, RESULT_TYPE ) \
                        case ws_request_type::METHOD: { \
                            auto params = request_params.as<chain_apis::read_write::METHOD##_params>(); \
                            rw_api.METHOD(std::move(params), [this, session, h](const chain::next_function_variant<chain_apis::read_write::RESULT_TYPE>& result) mutable { \
                                ws_response resp{ .request_id = h.request_id, .type = h.type }; \
                                \
                                if (std::holds_alternative<fc::exception_ptr>(result)) { \
                                    resp.error = std::get<fc::exception_ptr>(result)->to_detail_string(); \
                                    send_response(session, std::move(resp), fc::variant()); \
                                } else if (std::holds_alternative<chain_apis::read_write::RESULT_TYPE>(result)) { \
                                    fc::variant variant_response; \
                                    fc::to_variant(std::get<chain_apis::read_write::RESULT_TYPE>(result), variant_response); \
                                    send_response(session, std::move(resp), std::move(variant_response)); \
                                } else { \
                                    using rw_fwd_t = std::function<chain::t_or_exception<chain_apis::read_write::RESULT_TYPE>()>; \
                                    auto& http_plugin_pool = app().get_plugin<http_plugin>(); \
                                    \
                                    http_plugin_pool.post_http_thread_pool([this, session, resp = std::move(resp), fwd = std::get<rw_fwd_t>(std::move(result))]() mutable { \
                                        auto final_result = fwd(); \
                                        if (std::holds_alternative<fc::exception_ptr>(final_result)) { \
                                            resp.error = std::get<fc::exception_ptr>(final_result)->to_detail_string(); \
                                            send_response(session, std::move(resp), fc::variant()); \
                                        } else { \
                                            fc::variant variant_response; \
                                            fc::to_variant(std::get<chain_apis::read_write::RESULT_TYPE>(final_result), variant_response); \
                                            send_response(session, std::move(resp), std::move(variant_response)); \
                                        } \
                                    }); \
                                } \
                            }); \
                            return;\
                        }

                    switch (api_name) {
                        CALL_RO_API_NP(get_info)
                        CALL_RO_API_NP(get_activated_protocol_features)
                        CALL_RO_API_NP(get_consensus_parameters)
                        CALL_RO_API_NP(get_finalizer_info)
                        CALL_RO_API_NP(get_producer_schedule)

                        CALL_RO_API_P(get_abi)
                        CALL_RO_API_P(get_code)
                        CALL_RO_API_P(get_code_hash)
                        CALL_RO_API_P(get_raw_code_and_abi)
                        CALL_RO_API_P(get_raw_abi)
                        CALL_RO_API_P(get_table_by_scope)
                        CALL_RO_API_P(get_currency_balance)
                        CALL_RO_API_P(get_producers)
                        CALL_RO_API_P(get_scheduled_transactions)
                        CALL_RO_API_P(get_required_keys)
                        CALL_RO_API_P(get_transaction_id)
                        CALL_RO_API_P(get_raw_block)
                        CALL_RO_API_P(get_block_header)
                        CALL_RO_API_P(get_transaction_status)
                        CALL_RO_API_P(get_accounts_by_authorizers)

                        CALL_RO_API_P_V(get_block_info)
                        CALL_RO_API_P_V(get_block_header_state)
                        CALL_RO_API_P_V(get_currency_stats)

                        CALL_RO_API_P_E(get_table_rows, get_table_rows_result)
                        CALL_RO_API_P_E(get_account, get_account_results)

                        CALL_RO_API_P_E_V(get_block)

                        CALL_RO_API_ASYNC(compute_transaction, compute_transaction_results)
                        CALL_RO_API_ASYNC(send_read_only_transaction, send_read_only_transaction_results)

                        CALL_RW_API_ASYNC(push_transaction, push_transaction_results)
                        CALL_RW_API_ASYNC(push_transactions, push_transactions_results)
                        CALL_RW_API_ASYNC(send_transaction, send_transaction_results)
                        CALL_RW_API_ASYNC(send_transaction2, send_transaction_results)
                        
                        
                        default: {
                            resp.error = "API method not supported or not implemented yet";
                            break;
                        }
                    }
                    
                    #undef CALL_RO_API_NP
                    #undef CALL_RO_API_P
                    #undef CALL_RO_API_P_V
                    #undef CALL_RO_API_P_E
                    #undef CALL_RO_API_P_E_V
                    #undef CALL_RW_API_ASYNC
                    #undef CALL_RO_API_ASYNC

                    send_response(session, std::move(resp), std::move(variant_response));
                } catch (const fc::exception& e) {
                    resp.error = e.to_detail_string();
                    send_response(session, std::move(resp), std::move(fc::variant()));
                } catch (const std::exception& e) {
                    resp.error = e.what();
                    send_response(session, std::move(resp), std::move(fc::variant()));
                } catch (...) {
                    resp.error = "unknown exception";
                    send_response(session, std::move(resp), std::move(fc::variant()));
                }
            };

        private:
            fc::variant parse_json(const std::vector<char>& payload) {
                if (payload.empty()) return fc::variant{};
                return fc::json::from_string(std::string(payload.begin(), payload.end()));
            }

            void send_response(session_base* session, ws_response&& resp, fc::variant&& result) {
                if (resp.error.empty() && !result.is_null()) {
                    std::string json = fc::json::to_string(result, fc::time_point::maximum());
                    std::string compressed = fc::zlib_compress(json);
                    resp.result.assign(compressed.begin(), compressed.end());
                }

                session->post_to_strand([session, resp = std::move(resp)]() mutable {
                    session->send_response(resp);
                });
            }

        private:
            fc::microseconds max_response_time;
    
    };
}