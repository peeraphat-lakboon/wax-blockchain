#include <eosio/chain/thread_utils.hpp>  
#include <eosio/chain/types.hpp>  

#include <boost/asio/strand.hpp>               
#include <boost/asio/ip/tcp.hpp>               
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/signals2/connection.hpp>

#include <eosio/websocket_rpc_plugin/websocket_rpc_plugin.hpp>
#include <eosio/websocket_rpc_plugin/session.hpp>
#include <eosio/websocket_rpc_plugin/macros.hpp>

#include <fc/io/datastream.hpp>
#include <fc/network/listener.hpp>

#include <shared_mutex>

namespace eosio {

   namespace {
      inline fc::logger& logger() {
         static fc::logger log{ "websocket_rpc_plugin" };
         return log;
      }

      void handle_api_exception(const ws_response_callback& cb, const std::function<void()>& func) {
         try {
            func();
         } catch(const fc::exception& e) {
            error_results results{500, "Internal Service Error", error_results::error_info( e, true )};
            cb(fc::variant(results));
         } catch(const std::exception& e) {
            error_results results{500, "Internal Service Error", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, e.what() ) ), true )};
            cb(fc::variant(results));
         } catch (...) {
            error_results results{500, "Internal Service Error", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, "Unknown exception" ) ), true )};
            cb(fc::variant(results));
         }
      }
   }

   static auto _websocket_rpc_plugin = application::register_plugin<websocket_rpc_plugin>();

   using namespace std;
   using namespace eosio;
   using namespace eosio::chain;

   using boost::signals2::scoped_connection;

   using tcp_session = ws_rpc_session<boost::asio::ip::tcp::socket>;
   using unix_session = ws_rpc_session<boost::asio::local::stream_protocol::socket>;

   class websocket_rpc_plugin_impl : public std::enable_shared_from_this<websocket_rpc_plugin_impl> {
      public:
         chain_plugin*                         chain_plug = nullptr;

         std::string                           ws_address;
         std::string                           ws_unix;

         std::optional<scoped_connection>      block_start_connection;
         std::optional<scoped_connection>      accepted_block_connection;
         std::optional<scoped_connection>      applied_transaction_connection;

         std::optional<chain_apis::read_only>  ro_api;
         std::optional<chain_apis::read_write> rw_api;

         std::map<chain::transaction_id_type, cache_trace> cached_traces;
         std::optional<cache_trace>                        onblock_trace;

         std::shared_mutex connections_mtx;

         std::set<std::shared_ptr<tcp_session>, connection_map_key_less>  tcp_connections;
         std::set<std::shared_ptr<unix_session>, connection_map_key_less> unix_connections;

         std::shared_ptr<websocket_rpc_state> websocket_state{new websocket_rpc_state(logger())};

         ws_api_handler make_app_thread_handler(ws_handler&& next, appbase::exec_queue queue, int priority) {
            return ws_api_handler {
               [queue, priority, next = move(next)](string&& body, ws_response_callback&& cb) mutable {
                  app().executor().post(priority, queue, [next, body = move(body), cb = move(cb)]() mutable {
                     handle_api_exception(cb, [&]() {
                        if (app().is_quiting()) return;
                        next(move(body), move(cb));
                     });
                  });
               }
            };
         }

         ws_api_handler make_async_handler(ws_handler&& next) {
            return ws_api_handler {
               [next = move(next)](string&& body, ws_response_callback&& cb) mutable {
                  handle_api_exception(cb, [&]() {
                     next(move(body), move(cb));
                  });
               }
            };
         }

         void add_api(int id, ws_handler&& api_call, appbase::exec_queue queue = appbase::exec_queue::read_only, int priority = appbase::priority::medium_low) {
            websocket_state->api_handlers[id] = make_app_thread_handler(move(api_call), queue, priority);
         }

         void add_async_api(int id, ws_handler&& api_call) {
            websocket_state->api_handlers[id] = make_async_handler(move(api_call));
         }

         template <typename Protocol>
         void create_listener(const string& address) {
            const boost::posix_time::milliseconds accept_timeout(500);
            auto create_session = [this](Protocol::socket&& socket) {
               using SessionType = ws_rpc_session<decay_t<decltype(socket)>>;

               auto on_session_closed = [this](SessionType* session) {
                  boost::asio::post( websocket_state->thread_pool.get_executor(), [this, session]() {
                     std::unique_lock lock(connections_mtx);

                     if constexpr (std::is_same_v<typename Protocol::socket, boost::asio::ip::tcp::socket>) {
                        auto it = tcp_connections.find(session);
                        if(it != tcp_connections.end()) tcp_connections.erase(it);
                     } else {
                        auto it = unix_connections.find(session);
                        if(it != unix_connections.end()) unix_connections.erase(it);
                     }
                  });
               };

               auto new_sess = make_shared<SessionType>(
                  std::move(socket), std::move(on_session_closed), websocket_state
               );

               new_sess->run();

               {
                  std::unique_lock lock(connections_mtx);

                  if constexpr (std::is_same_v<typename Protocol::socket, boost::asio::ip::tcp::socket>) {
                     tcp_connections.emplace(std::move(new_sess));
                  } else {
                     unix_connections.emplace(std::move(new_sess));
                  }
               }
            };
            
            fc::create_listener<Protocol>(websocket_state->thread_pool.get_executor(), logger(), accept_timeout, address, "",
               [this](const auto&) { return boost::asio::make_strand(websocket_state->thread_pool.get_executor()); },
               create_session
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

         void broadcast_to_subscribers(const std::vector<char>& data) {
            if (data.empty()) return;

            std::shared_lock lock(connections_mtx);

            for (const auto& session : tcp_connections) {
               if (session->is_subscriber()) {
                  session->send_update(data);
               }
            }

            for (const auto& session : unix_connections) {
               if (session->is_subscriber()) {
                  session->send_update(data);
               }
            }
         }

         void append_table_deltas(const chainbase::database& db, fc::datastream<char*>& ds) {
            try {
               std::vector<table_delta> deltas;

               const auto& kv_index = db.get_index<chain::key_value_index>();
               const auto& tidx     = db.get_index<chain::table_id_multi_index>();

               const auto& kv_undo  = kv_index.last_undo_session();
               const auto& tid_undo = tidx.last_undo_session();

               std::map<uint64_t, const chain::table_id_object*> removed_table_id;
               for (auto& rem : tid_undo.removed_values)
                  removed_table_id[rem.id._id] = &rem;

               auto get_table = [&](uint64_t tid) -> const chain::table_id_object* {
                  if (const auto* t = tidx.find(tid)) return t;
                  if (auto it = removed_table_id.find(tid); it != removed_table_id.end()) return it->second;
                  return nullptr;
               };

               auto add_delta = [&](delta_action action, const chain::key_value_object& row, const chain::table_id_object& table, const chain::key_value_object* old_row = nullptr) {
                  table_delta delta{
                     .action = action,
                     .code = table.code,
                     .scope = table.scope,
                     .table = table.table,
                     .primary_key = row.primary_key,
                     .payer = row.payer,
                  };

                  delta.new_data.assign(row.value.begin(), row.value.end());
                  
                  if (action == delta_action::update && old_row) {
                     delta.old_data.assign(old_row->value.begin(), old_row->value.end());
                  }

                  deltas.push_back(std::move(delta));
               };

               /* --- Updates --- */ 
               for (const auto& old : kv_undo.old_values) {
                  if (const auto* row = kv_index.find(old.id)) {
                     if (const auto* table = get_table(row->t_id._id)) {
                        add_delta(delta_action::update, *row, *table, &old);
                     }
                  }
               }

               /* --- Erases --- */
               for (const auto& old : kv_undo.removed_values) {
                  if (const auto* table = get_table(old.t_id._id)) {
                     add_delta(delta_action::erase, old, *table);
                  }
               }

               /* --- Inserts --- */
               for (const auto& row : kv_undo.new_values) {
                  if (const auto* table = get_table(row.t_id._id)) {
                     add_delta(delta_action::insert, row, *table);
                  }
               }

               fc::raw::pack(ds, deltas);
               
            } catch (const std::exception& e) {
               fc_elog(logger(), "Error in append_table_deltas: ${e}", ("e", e.what()));
            }
         }

         void append_traces(const chain::signed_block_ptr& block, fc::datastream<char*>& ds) {
            std::vector<cache_trace> traces;
            
            if (cached_traces.empty() && !onblock_trace) {
               fc::raw::pack(ds, traces);
               return;
            }

            try {
               traces.reserve(cached_traces.size() + 1);
               
               if (onblock_trace) {
                  traces.push_back(*onblock_trace);
               }

               for (auto& r : block->transactions) {
                  chain::transaction_id_type id;
                  if (std::holds_alternative<chain::transaction_id_type>(r.trx)) {
                     id = std::get<chain::transaction_id_type>(r.trx);
                  } else {
                     id = std::get<chain::packed_transaction>(r.trx).id();
                  }
                  const auto it = cached_traces.find( id );
                  if( it != cached_traces.end() ) {
                     traces.push_back(it->second);
                  }
               }

               fc::raw::pack(ds, traces);

            } catch (const std::exception& e) {
               fc_elog(logger(), "Error in append_traces: ${e}", ("e", e.what()));
            }
         }

         void on_block_start( uint32_t block_num ) {
            clear_caches();
         }

         void on_applied_transaction(const transaction_trace_ptr& trace, const packed_transaction_ptr& transaction) {
            if (!trace->receipt || trace->receipt->status != chain::transaction_receipt_header::executed) {
               return;
            }

            if (chain::is_onblock(*trace)) {
               onblock_trace.emplace(trace, transaction);
            } else {
               cached_traces[trace->id] = cache_trace{trace, transaction};
            }
         }

         void on_accepted_block(const signed_block_ptr& block, const block_id_type& id) {
            try {
               std::vector<char> message_buffer(2 * 1024 * 1024);
               fc::datastream<char*> ds(message_buffer.data(), message_buffer.size());

               append_traces(block, ds);
               append_table_deltas(chain_plug->chain().db(), ds);

               message_buffer.resize(ds.tellp());

               if (!message_buffer.empty()) {
                  broadcast_to_subscribers(message_buffer);
               }

               clear_caches();

            } catch(const fc::exception& e) {
               fc_elog(logger(), "fc::exception: ${details}", ("details", e.to_detail_string()));
               appbase::app().quit();
            }
         }

         void clear_caches() {
            cached_traces.clear();
            onblock_trace.reset();
         }
   };

   websocket_rpc_plugin::websocket_rpc_plugin(): my(std::make_shared<websocket_rpc_plugin_impl>()) {}
   websocket_rpc_plugin::~websocket_rpc_plugin() = default;

   void websocket_rpc_plugin::set_program_options(options_description& cli, options_description& cfg) {
      auto options = cfg.add_options();

      options("ws-rpc-address", bpo::value<string>()->default_value("127.0.0.1:9090"), "Address and port for the websocket rpc listener.");
      options("ws-rpc-unix",    bpo::value<string>(), "Unix domain socket path (relative to data-dir) for the websocket rpc listener.");
      options("ws-rpc-threads", bpo::value<uint16_t>()->default_value( my->websocket_state->thread_pool_size ), "Number of worker threads in websocket rpc thread pool");
      options("ws-rpc-max-body-size", bpo::value<uint32_t>()->default_value( my->websocket_state->max_body_size ), "The maximum body size in bytes allowed for incoming RPC requests");
      options("ws-rpc-max-in-flight-requests", bpo::value<int32_t>()->default_value( my->websocket_state->max_requests_in_flight ), "The maximum number of requests in flight. -1 for unlimited. 503 error response when exceeded.");
   }

   void websocket_rpc_plugin::plugin_initialize(const variables_map& options) {
      handle_sighup();
      
      try {
         my->chain_plug = app().find_plugin<chain_plugin>();
         EOS_ASSERT(my->chain_plug, chain::missing_chain_plugin_exception, "");

         auto& chain = my->chain_plug->chain();

         my->applied_transaction_connection.emplace(
            chain.applied_transaction().connect([this](std::tuple<const chain::transaction_trace_ptr&, const chain::packed_transaction_ptr&> t) {
               my->on_applied_transaction(std::get<0>(t), std::get<1>(t));
            })
         );

         my->accepted_block_connection.emplace(
            chain.accepted_block().connect([&](const block_signal_params& t) {
               const auto& [ block, id ] = t;
               my->on_accepted_block(block, id);
            })
         );

         my->block_start_connection.emplace(
            chain.block_start().connect([&](uint32_t block_num) { 
               my->on_block_start(block_num); 
            })
         );

         my->ws_address = options.at("ws-rpc-address").as<string>();

         if (options.count("ws-rpc-unix")) {
            filesystem::path ws_unix_path = options.at("ws-rpc-unix").as<string>();
            if (ws_unix_path.is_relative())
               ws_unix_path = app().data_dir() / ws_unix_path;
            my->ws_unix = ws_unix_path.generic_string();
         }

         my->websocket_state->thread_pool_size = options.at( "ws-rpc-threads" ).as<uint16_t>();
         EOS_ASSERT( my->websocket_state->thread_pool_size > 0, chain::plugin_config_exception,
                     "ws-rpc-threads ${num} must be greater than 0", ("num", my->websocket_state->thread_pool_size));

         my->websocket_state->max_body_size = options.at( "ws-rpc-max-body-size" ).as<uint32_t>();
         my->websocket_state->max_requests_in_flight = options.at( "ws-rpc-max-in-flight-requests" ).as<int32_t>();
      }
      FC_LOG_AND_RETHROW()
   }

   void websocket_rpc_plugin::plugin_startup() {
      auto& chain = app().get_plugin<chain_plugin>();
      auto& _http_plugin = app().get_plugin<http_plugin>();
      fc::microseconds max_response_time = _http_plugin.get_max_response_time();

      my->ro_api.emplace(chain.get_read_only_api(max_response_time));
      my->rw_api.emplace(chain.get_read_write_api(max_response_time));
      
      auto& ro_api = *my->ro_api;
      auto& rw_api = *my->rw_api;

      my->add_async_api(2, RO_CALL(get_info, ro_api, http_params_types::no_params));
      my->add_api(3,  RO_CALL(get_activated_protocol_features, ro_api, http_params_types::no_params));
      my->add_api(4,  RO_CALL_POST(get_block, ro_api, fc::variant, http_params_types::params_required));
      my->add_api(5,  RO_CALL(get_block_info, ro_api, http_params_types::params_required));
      my->add_api(6,  RO_CALL(get_block_header_state, ro_api, http_params_types::params_required));
      my->add_api(7,  RO_CALL_POST(get_account, ro_api, chain_apis::read_only::get_account_results, http_params_types::params_required));
      my->add_api(8,  RO_CALL(get_code, ro_api, http_params_types::params_required));
      my->add_api(9,  RO_CALL(get_code_hash, ro_api, http_params_types::params_required));
      my->add_api(10, RO_CALL(get_consensus_parameters, ro_api, http_params_types::no_params));
      my->add_api(11, RO_CALL(get_abi, ro_api, http_params_types::params_required));
      my->add_api(12, RO_CALL(get_raw_code_and_abi, ro_api, http_params_types::params_required));
      my->add_api(13, RO_CALL(get_raw_abi, ro_api, http_params_types::params_required));
      my->add_api(14, RO_CALL(get_finalizer_info, ro_api, http_params_types::no_params));
      my->add_api(15, RO_CALL_POST(get_table_rows, ro_api, chain_apis::read_only::get_table_rows_result, http_params_types::params_required));
      my->add_api(16, RO_CALL(get_table_by_scope, ro_api, http_params_types::params_required));
      my->add_api(17, RO_CALL(get_currency_balance, ro_api, http_params_types::params_required));
      my->add_api(18, RO_CALL(get_currency_stats, ro_api, http_params_types::params_required));
      my->add_api(19, RO_CALL(get_producers, ro_api, http_params_types::params_required));
      my->add_api(20, RO_CALL(get_producer_schedule, ro_api, http_params_types::no_params));
      my->add_api(21, RO_CALL(get_scheduled_transactions, ro_api, http_params_types::params_required));
      my->add_api(22, RO_CALL(get_required_keys, ro_api, http_params_types::params_required));
      my->add_api(23, RO_CALL(get_transaction_id, ro_api, http_params_types::params_required));
      my->add_api(24, RO_CALL_ASYNC(compute_transaction, ro_api, chain_apis::read_only::compute_transaction_results, http_params_types::params_required));
      my->add_api(25, RW_CALL_ASYNC(push_transaction, rw_api, chain_apis::read_write::push_transaction_results, http_params_types::params_required));
      my->add_api(26, RW_CALL_ASYNC(push_transactions, rw_api, chain_apis::read_write::push_transactions_results, http_params_types::params_required));
      my->add_api(27, RW_CALL_ASYNC(send_transaction, rw_api, chain_apis::read_write::send_transaction_results, http_params_types::params_required));
      my->add_api(28, RW_CALL_ASYNC(send_transaction2, rw_api, chain_apis::read_write::send_transaction_results, http_params_types::params_required));
      my->add_api(29, RO_CALL(get_accounts_by_authorizers, ro_api, http_params_types::params_required));
      my->add_async_api(30, RO_CALL_ASYNC(send_read_only_transaction, ro_api, chain_apis::read_only::send_read_only_transaction_results, http_params_types::params_required));
      my->add_async_api(31, RO_CALL(get_raw_block, ro_api, http_params_types::params_required));
      my->add_async_api(32, RO_CALL(get_block_header, ro_api, http_params_types::params_required));
      my->add_api(33, RO_CALL(get_transaction_status, ro_api, http_params_types::params_required));

      my->websocket_state->thread_pool.start(my->websocket_state->thread_pool_size, [](const fc::exception& e) {
         fc_elog( logger(), "Exception in chain websocket thread pool, exiting: ${e}", ("e", e.to_detail_string()) );
         app().quit();
      });

      my->server_listen();

      fc_ilog( logger(), "websocket_rpc_plugin started listening on ${addr}, ${unixaddr}", ("addr", my->ws_address)("unixaddr", my->ws_unix));
   }

   void websocket_rpc_plugin::plugin_shutdown() {
      my->websocket_state->thread_pool.stop();

      {
         std::unique_lock lock(my->connections_mtx);
         
         my->tcp_connections.clear();
         my->unix_connections.clear();
      }

      fc_ilog( logger(), "websocket_rpc_plugin shutdown complete");
   }

   void websocket_rpc_plugin::post_thread_pool(std::function<void()> f) {
      if ( f ) boost::asio::post( my->websocket_state->thread_pool.get_executor(), f );
   }

   void websocket_rpc_plugin::handle_exception(const ws_response_callback& cb) {
      try {
         try {
            throw;
         } catch (chain::unknown_block_exception& e) {
            error_results results{400, "Unknown Block", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (chain::invalid_http_request& e) {
            error_results results{400, "Invalid Request", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (chain::account_query_exception& e) {
            error_results results{400, "Account lookup", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (chain::unsatisfied_authorization& e) {
            error_results results{401, "UnAuthorized", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (chain::tx_duplicate& e) {
            error_results results{409, "Conflict", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (fc::eof_exception& e) {
            error_results results{422, "Unprocessable Entity", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (fc::exception& e) {
            error_results results{500, "Internal Service Error", error_results::error_info(e, true)};
            cb( fc::variant( results ));
         } catch (std::exception& e) {
            error_results results{500, "Internal Service Error", error_results::error_info(fc::exception( FC_LOG_MESSAGE( error, e.what())), true)};
            cb( fc::variant( results ));
         } catch (...) {
            error_results results{500, "Internal Service Error", error_results::error_info(fc::exception( FC_LOG_MESSAGE( error, "Unknown Exception" )), true)};
            cb( fc::variant( results ));
         }
      } catch (...) {
         std::cerr << "Exception attempting to handle exception " << std::endl;
      }
   }

   void websocket_rpc_plugin::handle_sighup() {
      fc::logger::update( logger().get_name(), logger() );
   }

}