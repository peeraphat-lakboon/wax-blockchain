#pragma once

#include <eosio/chain/types.hpp>

namespace eosio::chain_ws {

    enum class ws_request_type : uint8_t {
        subscribe                       = 0,
        unsubscribe                     = 1,
        get_info                        = 2,
        get_activated_protocol_features = 3,
        get_block                       = 4,
        get_block_info                  = 5,
        get_block_header_state          = 6,
        get_account                     = 7,
        get_code                        = 8,
        get_code_hash                   = 9,
        get_consensus_parameters        = 10,
        get_abi                         = 11,
        get_raw_code_and_abi            = 12,
        get_raw_abi                     = 13,
        get_finalizer_info              = 14,
        get_table_rows                  = 15,
        get_table_by_scope              = 16,
        get_currency_balance            = 17,
        get_currency_stats              = 18,
        get_producers                   = 19,
        get_producer_schedule           = 20,
        get_scheduled_transactions      = 21,
        get_required_keys               = 22,
        get_transaction_id              = 23,
        compute_transaction             = 24,
        push_transaction                = 25,
        push_transactions               = 26,
        send_transaction                = 27,
        send_transaction2               = 28,
        get_accounts_by_authorizers     = 29,
        send_read_only_transaction      = 30,
        get_raw_block                   = 31,
        get_block_header                = 32,
        get_transaction_status          = 33
    };

    struct ws_header {
        string   request_id;
        uint32_t type;
        uint32_t size;
    };

    struct ws_response {
        string               request_id;
        uint32_t             type;
        string               error;
        std::vector<uint8_t> result;
    };

}

FC_REFLECT(eosio::chain_ws::ws_header, (request_id)(type)(size))
FC_REFLECT(eosio::chain_ws::ws_response, (request_id)(type)(error)(result))
FC_REFLECT_ENUM(eosio::chain_ws::ws_request_type, 
    (subscribe)(unsubscribe)
    (get_info)(get_activated_protocol_features)(get_block)(get_block_info)(get_block_header_state)
    (get_account)(get_code)(get_code_hash)(get_consensus_parameters)(get_abi)(get_raw_code_and_abi)(get_raw_abi)
    (get_finalizer_info)(get_table_rows)(get_table_by_scope)(get_currency_balance)(get_currency_stats)(get_producers)
    (get_producer_schedule)(get_scheduled_transactions)(get_required_keys)(get_transaction_id)(compute_transaction)
    (push_transaction)(push_transactions)(send_transaction)(send_transaction2)(get_accounts_by_authorizers)
    (send_read_only_transaction)(get_raw_block)(get_block_header)(get_transaction_status)
)
