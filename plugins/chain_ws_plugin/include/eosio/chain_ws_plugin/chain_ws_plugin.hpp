#pragma once
#include <eosio/chain/application.hpp>

#include <eosio/chain_plugin/chain_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>

namespace eosio {

   using namespace appbase;

   class chain_ws_plugin : public appbase::plugin<chain_ws_plugin> {
      public:
         APPBASE_PLUGIN_REQUIRES((chain_plugin)(http_plugin))

         chain_ws_plugin();
         virtual ~chain_ws_plugin();
      
         virtual void set_program_options(options_description&, options_description& cfg) override;
      
         void plugin_initialize(const variables_map& options);
         void plugin_startup();
         void plugin_shutdown();
         void handle_sighup() override;

      private:
         std::unique_ptr<class chain_ws_plugin_impl> my;
   };
}