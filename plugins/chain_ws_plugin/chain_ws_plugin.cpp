#include <eosio/chain/thread_utils.hpp>  
#include <eosio/chain/types.hpp>  

#include <boost/asio/strand.hpp>               
#include <boost/asio/ip/tcp.hpp>               
#include <boost/asio/local/stream_protocol.hpp>

#include <boost/signals2/connection.hpp>

#include <eosio/chain_ws_plugin/chain_ws_plugin.hpp>
#include <eosio/chain_ws_plugin/session.hpp>
#include <eosio/chain_ws_plugin/api_dispatcher.hpp>
#include <eosio/chain_ws_plugin/types.hpp>

#include <fc/network/listener.hpp>

#include <shared_mutex>

namespace eosio {

   namespace {
      inline fc::logger& logger() {
         static fc::logger log{ "chain_ws_plugin" };
         return log;
      }
   }

   static auto _chain_ws_plugin = application::register_plugin<chain_ws_plugin>();

   using namespace eosio;
   using namespace eosio::chain;
   using namespace eosio::chain_ws;
   using boost::signals2::scoped_connection;

   class chain_ws_plugin_impl {
      private:
         chain_plugin* chain_plug = nullptr;

         string                            ws_address;
         string                            ws_unix;
         
         named_thread_pool<struct chainws> thread_pool;
         uint16_t                          thread_pool_size = 4;

         struct connection_map_key_less {
            using is_transparent = void;
            template<typename L, typename R> bool operator()(const L& lhs, const R& rhs) const {
               return std::to_address(lhs) < std::to_address(rhs);
            }
         };

         // Use shared_mutex for thread-safe access to connections
         std::shared_mutex connections_mtx;
         std::set<std::unique_ptr<session_base>, connection_map_key_less> connections;

         std::unique_ptr<ws_api_dispatcher> api_dispatcher;

      public:
         void set_program_options(options_description& cfg);
         void plugin_initialize(const variables_map& options);
         void plugin_startup();
         void plugin_shutdown();

         template <typename Protocol>
         void create_listener(const std::string& address) {
            const boost::posix_time::milliseconds accept_timeout(200);
            fc::create_listener<Protocol>(thread_pool.get_executor(), logger(), accept_timeout, address, "",
               [this](const auto&) { return boost::asio::make_strand(thread_pool.get_executor()); },
               [this](Protocol::socket&& socket) {
                  auto on_session_closed = [this](session_base* conn) {
                     // Need to post to app executor to safely remove from set, 
                     // but must lock because listener runs on thread pool
                     app().executor().post(priority::high, exec_queue::read_write, [this, conn]() {
                        std::unique_lock lock(connections_mtx);
                        auto it = connections.find(conn);
                        if(it != connections.end()) {
                           connections.erase(it);
                        }
                     });
                  };

                  auto on_call_ws_api = [this](session_base* conn, ws_header h, std::vector<char> payload) {
                     app().executor().post(priority::medium_low, exec_queue::read_only, [this, conn, h, payload]() {
                        ws_request_type req_type = static_cast<ws_request_type>(h.type);
                        api_dispatcher->call(req_type, conn, h, payload);
                     });
                  };

                  auto new_sess = std::make_unique<ws_rpc_session<std::decay_t<decltype(socket)>, decltype(on_session_closed), decltype(on_call_ws_api)>>(
                     std::move(socket),
                     std::move(on_session_closed),
                     std::move(on_call_ws_api),
                     logger()
                  );

                  {
                     std::unique_lock lock(connections_mtx);
                     connections.emplace(std::move(new_sess));
                  }
               }
            );
         }

         void server_listen() {
            try {
               if(!ws_address.empty())
                  create_listener<boost::asio::ip::tcp>(ws_address);
               if(!ws_unix.empty())
                  create_listener<boost::asio::local::stream_protocol>(ws_unix);
            } catch(std::exception& e) {
               FC_THROW_EXCEPTION(plugin_exception, "Unable to open listen socket: ${e}", ("e", e.what()));
            }
         }
   };

   chain_ws_plugin::chain_ws_plugin(): my(std::make_unique<chain_ws_plugin_impl>()) {}
   chain_ws_plugin::~chain_ws_plugin() = default;

   void chain_ws_plugin_impl::set_program_options(options_description& cfg) {
      auto options = cfg.add_options();

      options("chain-ws-address", 
         bpo::value<string>()->default_value("127.0.0.1:9090"),
         "Address and port for the chain websocket listener."   
      );
      options("chain-ws-unix", 
         bpo::value<string>(),
         "Unix domain socket path (relative to data-dir) for the chain websocket listener."
      );
      options("chain-ws-threads", 
         bpo::value<uint16_t>()->default_value( thread_pool_size ),
         "Number of worker threads in chain websocket thread pool"
      );
   }

   void chain_ws_plugin_impl::plugin_initialize(const variables_map& options) {
      try {
         chain_plug = app().find_plugin<chain_plugin>();
         EOS_ASSERT(chain_plug, chain::missing_chain_plugin_exception, "");

         ws_address = options.at("chain-ws-address").as<string>();

         if (options.count("chain-ws-unix")) {
            std::filesystem::path ws_unix_path = options.at("chain-ws-unix").as<string>();
            if (ws_unix_path.is_relative())
               ws_unix_path = app().data_dir() / ws_unix_path;
            ws_unix = ws_unix_path.generic_string();
         }

         thread_pool_size = options.at( "chain-ws-threads" ).as<uint16_t>();
         EOS_ASSERT( thread_pool_size > 0, chain::plugin_config_exception,
                     "chain-ws-threads ${num} must be greater than 0", ("num", thread_pool_size));
      }
      FC_LOG_AND_RETHROW()
   }

   void chain_ws_plugin_impl::plugin_startup() {
      auto& _http_plugin = app().get_plugin<http_plugin>();
      fc::microseconds max_response_time = _http_plugin.get_max_response_time();
      api_dispatcher = std::make_unique<ws_api_dispatcher>(max_response_time);

      server_listen();
      
      thread_pool.start(thread_pool_size, [](const fc::exception& e) {
         fc_elog( logger(), "Exception in chain websocket thread pool, exiting: ${e}", ("e", e.to_detail_string()) );
         app().quit();
      });

      fc_ilog(logger(), "chain_ws_plugin started listening on ${addr}", ("addr", ws_address));
   }

   void chain_ws_plugin_impl::plugin_shutdown() {
      thread_pool.stop();
      
      {
         std::unique_lock lock(connections_mtx);
         connections.clear();
      }

      fc_dlog( logger(), "chain_ws_plugin shutdown complete");
   }

   void chain_ws_plugin::set_program_options(options_description& cli, options_description& cfg) {
      my->set_program_options(cfg);
   }

   void chain_ws_plugin::plugin_initialize(const variables_map& options) {
      handle_sighup();
      my->plugin_initialize(options);
   }

   void chain_ws_plugin::plugin_startup() {
      my->plugin_startup();
   }

   void chain_ws_plugin::plugin_shutdown() {
      my->plugin_shutdown();
   }

   void chain_ws_plugin::handle_sighup() {
      fc::logger::update( logger().get_name(), logger() );
   }

}