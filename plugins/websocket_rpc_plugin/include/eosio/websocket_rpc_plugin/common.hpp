#pragma once

#include <eosio/chain/thread_utils.hpp>
#include <eosio/http_plugin/http_plugin.hpp>

#include <fc/log/logger_config.hpp>

namespace eosio {

    struct websocket_rpc_state {
        std::map<int, ws_api_handler>                   api_handlers;

        eosio::chain::named_thread_pool<struct wsrpc>   thread_pool;
        uint16_t                                        thread_pool_size = 2;

        size_t                                          max_body_size{2 * 1024 * 1024};
        std::atomic<int32_t>                            requests_in_flight{0};
        int32_t                                         max_requests_in_flight = -1;

        fc::logger&                                     logger;

        explicit websocket_rpc_state(fc::logger& log) : logger(log) {}

        fc::logger& get_logger() { 
            return logger; 
        }

        bool is_overloaded() const {
            if (max_requests_in_flight < 0) return false;
            return requests_in_flight.load(std::memory_order_relaxed) >= max_requests_in_flight;
        }
    };

    class RequestGuard {
        std::shared_ptr<websocket_rpc_state> state_;
        
    public:
        explicit RequestGuard(std::shared_ptr<websocket_rpc_state> state)
            : state_(std::move(state)) 
        {
            state_->requests_in_flight.fetch_add(1, std::memory_order_relaxed);
        }
        ~RequestGuard() {
            state_->requests_in_flight.fetch_sub(1, std::memory_order_relaxed);
        }

        RequestGuard(const RequestGuard&) = delete;
        RequestGuard& operator=(const RequestGuard&) = delete;
    };
}