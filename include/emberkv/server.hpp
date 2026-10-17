#pragma once

// Multi-reactor TCP server.
//
//   acceptor thread ──accept()──┐
//                               │ round-robin hand-off (mutex-guarded inbox + wake-up socket)
//            ┌──────────────────┼──────────────────┐
//            ▼                  ▼                  ▼
//      reactor 0          reactor 1   ...    reactor N-1
//   (own Poller, owns     (each connection lives on exactly one reactor
//    its connections)      for its whole life: no locking per connection)
//            │                  │                  │
//            └──────── CommandProcessor ── Store (sharded locks) ───┘
//
// Each reactor reads, parses every complete request in its buffer
// (pipelining), executes them and writes all replies back in one batch.

#include "emberkv/commands.hpp"
#include "emberkv/net.hpp"
#include "emberkv/resp.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace emberkv {

struct ServerConfig {
    std::string bind = "127.0.0.1";
    std::uint16_t port = 6380;  // 6379 is Redis; stay out of its way
    std::size_t io_threads = 0;  // 0 = one per hardware thread
    std::size_t max_clients = 10'000;
    /// Pending reply bytes per client. Beyond this we stop reading from that
    /// client until it drains its socket, so a slow reader can't make the
    /// server buffer without bound.
    std::size_t max_output_buffer = 64u * 1024 * 1024;
    resp::RequestParser::Limits limits{};
};

class Server {
public:
    Server(ServerConfig config, CommandProcessor& processor, ServerCounters& counters);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    /// Binds, then starts the acceptor and reactor threads. Returns immediately.
    /// Throws std::system_error if the address can't be bound.
    void start();

    /// Stops accepting, closes every connection and joins all threads. Idempotent.
    void stop() noexcept;

    /// The bound port (useful when configured with port 0).
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] std::size_t io_threads() const noexcept { return reactors_.size(); }

private:
    class Reactor;
    void accept_loop(std::stop_token stop);
    void accept_pending();

    ServerConfig config_;
    CommandProcessor& processor_;
    ServerCounters& counters_;

    net::Socket listener_;
    std::uint16_t port_ = 0;
    net::Socket wake_rx_;
    net::Socket wake_tx_;
    std::vector<std::unique_ptr<Reactor>> reactors_;
    std::size_t next_reactor_ = 0;
    std::jthread acceptor_;
};

}  // namespace emberkv
