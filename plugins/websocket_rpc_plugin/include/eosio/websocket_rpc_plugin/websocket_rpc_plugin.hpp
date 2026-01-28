#pragma once
#include <eosio/chain/application.hpp>
#include <eosio/chain/types.hpp>
#include <eosio/chain/trace.hpp>

#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>

namespace eosio {

   using namespace appbase;

   using ws_response_callback = std::function<void(std::optional<fc::variant>)>;
   using ws_handler = std::function<void(std::string&&, ws_response_callback&&)>;

   struct ws_api_handler {
      ws_handler   fn;
   };

   struct ws_header {
      std::string  request_id;
      uint32_t     type;
      uint32_t     size;
   };

   enum class delta_action {
      insert = 0,
      update = 1,
      erase  = 2
   };

   struct table_delta {
      delta_action         action;
      chain::name          code;
      chain::name          scope;
      chain::name          table;
      uint64_t             primary_key;
      chain::name          payer;
      chain::bytes         new_data;
      chain::bytes         old_data;
   };

   struct cache_trace {
      chain::transaction_trace_ptr        trace;
      chain::packed_transaction_ptr       trx;
   };

   struct connection_map_key_less {
      using is_transparent = void;
      template<typename L, typename R> bool operator()(const L& lhs, const R& rhs) const {
         return std::to_address(lhs) < std::to_address(rhs);
      }
   };

   class websocket_rpc_plugin : public appbase::plugin<websocket_rpc_plugin> {
      public:
         APPBASE_PLUGIN_REQUIRES((chain_plugin)(http_plugin))

         websocket_rpc_plugin();
         virtual ~websocket_rpc_plugin();
      
         virtual void set_program_options(options_description&, options_description& cfg) override;
      
         void plugin_initialize(const variables_map& options);
         void plugin_startup();
         void plugin_shutdown();
         void handle_sighup() override;

         void post_thread_pool(std::function<void()> f);

         static void handle_exception( const ws_response_callback& cb );

      private:
         std::shared_ptr<class websocket_rpc_plugin_impl> my;
   };
}

FC_REFLECT(eosio::ws_header, (request_id)(type)(size))
FC_REFLECT(eosio::table_delta, (action)(code)(scope)(table)(primary_key)(payer)(new_data)(old_data))
FC_REFLECT(eosio::cache_trace, (trace)(trx))

FC_REFLECT_ENUM(eosio::delta_action, (insert)(update)(erase))