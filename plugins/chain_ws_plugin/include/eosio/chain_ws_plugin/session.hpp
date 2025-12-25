#pragma once

#include <eosio/chain/types.hpp>
#include <eosio/chain/controller.hpp>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/error.hpp>
#include <boost/beast/websocket.hpp>

#include <eosio/chain_ws_plugin/types.hpp>
#include <fc/compress/zlib.hpp>

namespace eosio::chain_ws {

    class session_base {
        public:
            session_base() = default;
            session_base(const session_base&) = delete;
            session_base& operator=(const session_base&) = delete;

            virtual void send_response(ws_response resp) = 0;
            virtual void post_to_strand(std::function<void()> fn) = 0;

            virtual ~session_base() = default;
    };

    template<typename SocketType, typename OnSessionClosed, typename OnCallWsAPI>
    requires std::is_same_v<SocketType, boost::asio::ip::tcp::socket> || std::is_same_v<SocketType, boost::asio::local::stream_protocol::socket>
    class ws_rpc_session final : public session_base {
        using websocket_stream = boost::asio::use_awaitable_t<>::as_default_on_t<boost::beast::websocket::stream<SocketType>>;
        using wakeup_timer = boost::asio::as_tuple_t<boost::asio::use_awaitable_t<>>::as_default_on_t<boost::asio::steady_timer>;

        public:
            ws_rpc_session(SocketType&& s, OnSessionClosed&& on_session_closed, OnCallWsAPI&& on_call_ws_api, fc::logger& logger) :
            session_strand(s.get_executor()), stream(std::move(s)), 
            write_wakeup_timer(session_strand), ping_timer(session_strand),
            on_session_closed(std::move(on_session_closed)), on_call_ws_api(std::move(on_call_ws_api)),
            logger(logger), remote_endpoint_string(get_remote_endpoint_string()) {
                fc_ilog(logger, "incoming chain websocket rpc connection from ${a}", ("a", remote_endpoint_string));

                boost::asio::co_spawn(
                    session_strand,
                    run_session_guarded([this]() -> boost::asio::awaitable<void> {
                        co_await run_session();
                    }),
                    boost::asio::detached
                );
            }

            void send_response(ws_response resp) override {
                boost::asio::co_spawn(session_strand,
                    [this, r = std::move(resp)]() mutable -> boost::asio::awaitable<void> {
                        if (closing.load()) co_return;

                        if (pending_responses.size() >= max_pending) {
                            fc_elog(logger, "Client ${a} queue full (${n}), disconnecting", ("a", remote_endpoint_string)("n", pending_responses.size()));
                            close_session(); 
                            co_return;
                        }

                        pending_responses.emplace_back(std::move(r));
                        write_wakeup_timer.cancel_one();
                    },
                    boost::asio::detached
                );
            }

            void post_to_strand(std::function<void()> fn) override {
                if (closing.load()) return;
                boost::asio::post(session_strand, std::move(fn));
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
                if constexpr(std::is_same_v<SocketType, boost::asio::ip::tcp::socket>)
                    stream.next_layer().set_option(boost::asio::ip::tcp::no_delay(true));

                stream.next_layer().set_option(
                    boost::asio::socket_base::send_buffer_size(1024 * 1024)
                );
                
                stream.set_option(boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));

                stream.set_option(
                    boost::beast::websocket::stream_base::decorator(
                        [](boost::beast::websocket::response_type& res) {
                            res.set(boost::beast::http::field::server, "chain_ws");
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
                    while (stream.is_open() && !closing.load()) {
                        boost::beast::flat_buffer b;
                        co_await stream.async_read(b);
                        on_frame(b);
                    }
                });
            }

            boost::asio::awaitable<void> write_loop() {
                co_await run_session_guarded(
                    [this]() -> boost::asio::awaitable<void> {
                        while (stream.is_open() && !closing.load()) {
                            if (pending_responses.empty()) {
                                write_wakeup_timer.expires_at(std::chrono::steady_clock::time_point::max());
                                co_await write_wakeup_timer.async_wait();
                            }
                            
                            if (closing.load() || !stream.is_open()) break;

                            while (!pending_responses.empty()) {
                                ws_response resp = std::move(pending_responses.front());
                                pending_responses.pop_front();
                                co_await write_response(std::move(resp));
                            }
                        }
                    }
                );
            }

            boost::asio::awaitable<void> ping_loop() {
                co_await run_session_guarded([this]() -> boost::asio::awaitable<void> {
                    while (stream.is_open() && !closing.load()) {
                        ping_timer.expires_after(std::chrono::seconds(30));
                        co_await ping_timer.async_wait();
                        if (stream.is_open()) 
                            co_await stream.async_ping({}, boost::asio::use_awaitable);
                    }
                });
            }

        private:
            void on_frame(const boost::beast::flat_buffer& b) {
                auto* data = static_cast<const char*>(b.cdata().data());
                auto  size = b.size();
                fc::datastream<const char*> ds(data, size);
                ws_header h;
                fc::raw::unpack(ds, h);
                if (!validate_request_header(h, ds)) {
                    reply_error(h, "bad request");
                    return;
                }
                std::vector<char> payload(h.size);
                if (h.size > 0) ds.read(payload.data(), h.size);
                on_call_ws_api(this, h, std::move(payload));
            }

            bool validate_request_header(const ws_header& h, fc::datastream<const char*>& ds) {
                return h.size <= ds.remaining();
            }

            void reply_error(const ws_header& h, std::string_view msg) {
                ws_response resp;
                resp.request_id = h.request_id;
                resp.type       = h.type;
                resp.error      = std::string(msg);
                send_response(std::move(resp));
            }

            boost::asio::awaitable<void> write_response(ws_response resp) {
                if (closing.load() || !stream.is_open()) co_return;
                try {
                    size_t sz = fc::raw::pack_size(resp);
                    std::vector<char> buffer(sz);
                    fc::datastream<char*> ds(buffer.data(), buffer.size());
                    fc::raw::pack(ds, resp);
                    co_await stream.async_write(boost::asio::buffer(buffer.data(), ds.tellp()));
                } catch(...) { co_return; }
            }

            std::string get_remote_endpoint_string() const {
                try {
                    if constexpr(std::is_same_v<SocketType, boost::asio::ip::tcp::socket>)
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

                boost::asio::co_spawn(session_strand, [this]() -> boost::asio::awaitable<void> {
                    try {
                        if (stream.is_open())
                            co_await stream.async_close(boost::beast::websocket::close_code::normal);
                    } catch(...) {}
                }, boost::asio::detached);

                drop_exceptions([&]{ stream.next_layer().close(); });

                app().executor().post(priority::high, exec_queue::read_write, [cb = on_session_closed, self = this]() {
                    self->pending_responses.clear();
                    cb(self);
                });
            }

        private:
            SocketType::executor_type         session_strand;
            websocket_stream                  stream;
            wakeup_timer                      write_wakeup_timer;
            wakeup_timer                      ping_timer;

            std::deque<ws_response>           pending_responses;
            std::atomic_flag                  has_logged_exception = ATOMIC_FLAG_INIT;

            OnSessionClosed                   on_session_closed;
            OnCallWsAPI                       on_call_ws_api;

            fc::logger&                       logger;
            const std::string                 remote_endpoint_string;

            std::atomic<bool>                 closing = false;

            static constexpr size_t           max_pending = 1024;
    };
}