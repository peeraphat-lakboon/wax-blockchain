#pragma once

#include <eosio/chain/types.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/websocket.hpp>

#include <eosio/websocket_rpc_plugin/websocket_rpc_plugin.hpp>
#include <eosio/websocket_rpc_plugin/common.hpp>

#include <eosio/http_plugin/http_plugin.hpp>

#include <fc/io/raw.hpp>

namespace eosio {
    struct queued_message {
        std::string header;
        std::variant<std::string, std::vector<char>> body;
    };

    struct buffered_request {
        ws_header h;
        std::string body;
        std::unique_ptr<RequestGuard> guard;
    };

    template<typename SocketType>
    requires std::is_same_v<SocketType, boost::asio::ip::tcp::socket> || std::is_same_v<SocketType, boost::asio::local::stream_protocol::socket>
    class ws_rpc_session final : public std::enable_shared_from_this<ws_rpc_session<SocketType>> {
        using websocket_stream = boost::asio::use_awaitable_t<>::as_default_on_t<boost::beast::websocket::stream<SocketType>>;
        using wakeup_timer = boost::asio::as_tuple_t<boost::asio::use_awaitable_t<>>::as_default_on_t<boost::asio::steady_timer>;

        using close_callback_type = std::function<void(ws_rpc_session*)>;

        public:
            ws_rpc_session(SocketType&& s, close_callback_type&& on_session_closed, std::shared_ptr<websocket_rpc_state> websocket_state) :
            session_strand(s.get_executor()), stream(std::move(s)), 
            write_wakeup_timer(session_strand), ping_timer(session_strand),
            on_session_closed(std::move(on_session_closed)), websocket_state_(std::move(websocket_state)), 
            remote_endpoint_string(get_remote_endpoint_string()) {
                fc_ilog(websocket_state_->get_logger(), "incoming chain websocket rpc connection from ${a}", ("a", remote_endpoint_string));
            }

            ~ws_rpc_session() {
                fc_ilog(websocket_state_->get_logger(), "closed connection from ${a}", ("a", remote_endpoint_string));
            }

            void run() {
                boost::asio::co_spawn(
                    session_strand,
                    run_session_guarded([self = this->shared_from_this()]() -> boost::asio::awaitable<void> {
                        co_await self->run_session();
                    }),
                    boost::asio::detached
                );
            }

            void set_subscriber() {
                subscriber.store(true);
            }

            void set_unsubscriber() {
                subscriber.store(false);
            }

            bool is_subscriber() const {
                return subscriber.load();
            }

            void send_update(std::vector<char> data) {
                if (closing.load()) return;

                ws_header h;
                h.type = 0;
                h.size = data.size();
                h.request_id = "subscribe"; 

                std::string h_packed;
                h_packed.resize(fc::raw::pack_size(h));
                fc::datastream<char*> ds(h_packed.data(), h_packed.size());
                fc::raw::pack(ds, h);

                boost::asio::dispatch(session_strand, [self = this->shared_from_this(), hp = std::move(h_packed), b = std::move(data)]() mutable {
                    if (self->closing.load()) return;

                    self->pending_responses.emplace_back(queued_message{std::move(hp), std::move(b)});
                    self->write_wakeup_timer.cancel_one();
                });
            }

        private:
            boost::asio::awaitable<void> run_session() {
                configure_websocket();
                co_await accept_connection();

                boost::asio::co_spawn(session_strand, write_loop(), boost::asio::detached);
                boost::asio::co_spawn(session_strand, ping_loop(), boost::asio::detached);

                co_await read_loop();
            }

        private:
            void configure_websocket() {
                if constexpr (std::is_same_v<SocketType, boost::asio::ip::tcp::socket>) {
                    stream.next_layer().set_option(boost::asio::ip::tcp::no_delay(true));
                }

                stream.next_layer().set_option(boost::asio::socket_base::send_buffer_size(SOCKET_BUFFER_SIZE));
                stream.next_layer().set_option(boost::asio::socket_base::receive_buffer_size(SOCKET_BUFFER_SIZE));

                stream.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
                stream.set_option(
                    boost::beast::websocket::stream_base::decorator(
                        [](boost::beast::websocket::response_type& res) {
                            res.set(boost::beast::http::field::server, "websocket_rpc");
                        }
                    )
                );
            }

            boost::asio::awaitable<void> accept_connection() {
                co_await stream.async_accept();
                stream.binary(true);
            }

        private:
            boost::asio::awaitable<void> read_loop() {
                co_await run_session_guarded([this]() -> boost::asio::awaitable<void> {
                    boost::beast::flat_buffer b;

                    while (stream.is_open() && !closing.load()) {
                        co_await stream.async_read(b, boost::asio::use_awaitable);
                        size_t consumed = on_frame(b);
                        b.consume(consumed);
                    }
                });
            }

            boost::asio::awaitable<void> write_loop() {
                co_await run_session_guarded([this]() -> boost::asio::awaitable<void> {
                    std::vector<queued_message> outgoing_batch;
                    std::vector<boost::asio::const_buffer> bufs;

                    outgoing_batch.reserve(512);
                    bufs.reserve(1024);

                    while (stream.is_open() && !closing.load()) {
                        if (pending_responses.empty()) {
                            write_wakeup_timer.expires_at(std::chrono::steady_clock::time_point::max());
                            try { co_await write_wakeup_timer.async_wait(); } catch(...) {}
                        }
                        
                        if (closing.load() || !stream.is_open()) break;
                        if (pending_responses.empty()) continue;

                        outgoing_batch.clear();
                        bufs.clear();

                        size_t total_bytes = 0;

                        while (!pending_responses.empty() && outgoing_batch.size() < MAX_WRITE_BATCH_COUNT) {
                            auto& next = pending_responses.front();
                            size_t body_size = std::visit([](const auto& b){ return b.size(); }, next.body);
                            size_t msg_size = next.header.size() + body_size;
                            
                            if (total_bytes > 0 && total_bytes + msg_size > MAX_WRITE_BATCH_BYTES) break;
                            
                            outgoing_batch.push_back(std::move(next));
                            pending_responses.pop_front();
                            total_bytes += msg_size;
                        }

                        for (auto& msg : outgoing_batch) {
                            bufs.emplace_back(boost::asio::buffer(msg.header));
                            
                            if (std::holds_alternative<std::string>(msg.body)) {
                                bufs.emplace_back(boost::asio::buffer(std::get<std::string>(msg.body)));
                            } else {
                                bufs.emplace_back(boost::asio::buffer(std::get<std::vector<char>>(msg.body)));
                            }
                        }

                        if (!bufs.empty()) {
                            co_await stream.async_write(bufs, boost::asio::use_awaitable);
                        }
                    }
                });
            }

            boost::asio::awaitable<void> ping_loop() {
                co_await run_session_guarded([this]() -> boost::asio::awaitable<void> {
                    while (stream.is_open() && !closing.load()) {
                        ping_timer.expires_after(PING_INTERVAL);
                        co_await ping_timer.async_wait();
                        if (stream.is_open()) 
                            co_await stream.async_ping({}, boost::asio::use_awaitable);
                    }
                });
            }

        private:
            size_t on_frame(const boost::beast::flat_buffer& b) {
                auto* data = static_cast<const char*>(b.cdata().data());
                auto  remaining_bytes = b.size();
                
                fc::datastream<const char*> ds(data, remaining_bytes);
                size_t fully_consumed_bytes = 0;

                std::vector<buffered_request> batch;
                batch.reserve(64);

                while (ds.remaining() > 0) {
                    ws_header h;
                    try {
                        fc::raw::unpack(ds, h);
                    } catch (...) {
                        break; 
                    }

                    if (h.size > ds.remaining()) {
                        break;
                    }

                    if (h.type == 0) { 
                        set_subscriber();
                        
                        ds.skip(h.size);
                        fully_consumed_bytes = remaining_bytes - ds.remaining();
                        continue;
                    } else if (h.type == 1) {
                        set_unsubscriber();

                        ds.skip(h.size);
                        fully_consumed_bytes = remaining_bytes - ds.remaining();
                        continue;
                    }

                    if (h.size > websocket_state_->max_body_size) {
                        error_results results{400, "Bad Request", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, "Request body size exceeded limit" ) ), true )};
                        enqueue_response(h, fc::variant(results));
                        
                        ds.skip(h.size); 
                    } else if (websocket_state_->is_overloaded()) {
                        error_results results{503, "Service Unavailable", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, "Too many requests in flight" ) ), true )};
                        enqueue_response(h, fc::variant(results));
                        
                        ds.skip(h.size);
                    } else {
                        auto guard = std::make_unique<RequestGuard>(websocket_state_);

                        std::string body;
                        body.resize(h.size);
                        if (h.size > 0) ds.read(body.data(), h.size);

                        batch.emplace_back(buffered_request{h, std::move(body), std::move(guard)});
                    }

                    fully_consumed_bytes = remaining_bytes - ds.remaining();
                }

                for (auto& job : batch) {
                    handle_api_request(job.h, std::move(job.body), std::move(job.guard));
                }

                return fully_consumed_bytes;
            }

            std::string get_remote_endpoint_string() const {
                try {
                    if constexpr (std::is_same_v<SocketType, boost::asio::ip::tcp::socket>)
                        return boost::lexical_cast<std::string>(stream.next_layer().remote_endpoint());
                    return "UNIX socket";
                } catch (...) { return "(unknown)"; }
            }
            
            template<typename F> 
            void drop_exceptions(F&& f) { try{ f(); } catch(...) {} }

            template<typename F>
            boost::asio::awaitable<void> run_session_guarded(F&& f) {
                try { co_await f(); }
                catch(...) { close_session(); }
            }

            void close_session() {
                bool expected = false;
                if (!closing.compare_exchange_strong(expected, true)) return;

                drop_exceptions([&]{ write_wakeup_timer.cancel(); });
                drop_exceptions([&]{ ping_timer.cancel(); });
                drop_exceptions([&]{ stream.next_layer().close(); });

                if (on_session_closed) {
                    on_session_closed(this);
                }
            }

        private:
            void handle_api_request(ws_header h, std::string body, std::unique_ptr<RequestGuard> guard) {
                try {
                    auto handler_itr = websocket_state_->api_handlers.find(h.type);

                    if (handler_itr != websocket_state_->api_handlers.end()) {
                        auto weak_self = std::weak_ptr<ws_rpc_session<SocketType>>(this->shared_from_this());

                        std::shared_ptr<RequestGuard> shared_guard = std::move(guard);

                        auto response_callback = [weak_self, h, shared_guard](std::optional<fc::variant> resp) mutable {
                            auto self = weak_self.lock();
                            if (!self) return;

                            self->enqueue_response(h, std::move(resp));
                        };

                        auto handler = handler_itr->second;

                        handler.fn(std::move(body), std::move(response_callback));

                    } else {
                        error_results results{404, "Not Found", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, "Unknown Endpoint" ) ), true )};
                        enqueue_response(h, fc::variant(results));
                    }

                } catch(const fc::exception& e) {
                    error_results results{500, "Internal Service Error", error_results::error_info( e, http_plugin::verbose_errors() )};
                    enqueue_response(h, fc::variant(results));
                } catch(const std::exception& e) {
                    error_results results{500, "Internal Service Error", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, e.what() ) ), true )};
                    enqueue_response(h, fc::variant(results));
                } catch (...) {
                    error_results results{500, "Internal Service Error", error_results::error_info( fc::exception( FC_LOG_MESSAGE( error, "Unknown exception" ) ), true )};
                    enqueue_response(h, fc::variant(results));
                }
            }

            void enqueue_response(ws_header h, std::optional<fc::variant> v) {
                if (closing.load()) return;
                
                std::string body_json = v.has_value() ? fc::json::to_string(*v, fc::time_point::maximum()) : "{}";
                h.size = body_json.size();

                std::string h_packed;
                h_packed.resize(fc::raw::pack_size(h));
                fc::datastream<char*> ds(h_packed.data(), h_packed.size());
                fc::raw::pack(ds, h);

                boost::asio::dispatch(session_strand, [self = this->shared_from_this(), hp = std::move(h_packed), bj = std::move(body_json)]() mutable {
                    if (self->closing.load()) return;

                    self->pending_responses.emplace_back(queued_message{std::move(hp), std::move(bj)});
                    self->write_wakeup_timer.cancel_one();
                });
            }

        private:
            SocketType::executor_type                       session_strand;
            websocket_stream                                stream;

            wakeup_timer                                    write_wakeup_timer;
            wakeup_timer                                    ping_timer;

            std::deque<queued_message>                      pending_responses;
            close_callback_type                             on_session_closed;
            std::shared_ptr<websocket_rpc_state>            websocket_state_;
            const std::string                               remote_endpoint_string;

            std::atomic<bool>                               closing = false;
            std::atomic<bool>                               subscriber = false;

            static constexpr size_t                         SOCKET_BUFFER_SIZE = 1024 * 1024;
            static constexpr size_t                         MAX_WRITE_BATCH_BYTES = 2 * 1024 * 1024;
            static constexpr size_t                         MAX_WRITE_BATCH_COUNT = 512;
            static constexpr auto                           PING_INTERVAL = std::chrono::seconds(30);
    };
}