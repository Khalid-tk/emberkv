#include "emberkv/server.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace emberkv {

using namespace std::chrono_literals;

namespace {

bool is_quit(std::string_view name) noexcept {
    constexpr std::string_view kQuit = "QUIT";
    return name.size() == kQuit.size() && std::ranges::equal(name, kQuit, [](char a, char b) {
               return std::toupper(static_cast<unsigned char>(a)) == b;
           });
}

void drain(net::native_socket s) noexcept {
    char buf[64];
    while (net::recv_some(s, buf, sizeof buf).status == net::IoResult::Status::ok) {
    }
}

void wake(net::native_socket s) noexcept {
    // If the socket buffer is full a wake-up is already pending: ignoring
    // would_block here is correct, not sloppy.
    (void)net::send_some(s, "x");
}

}  // namespace

// ------------------------------------------------------------------ reactor

struct Connection {
    Connection(net::Socket s, const resp::RequestParser::Limits& limits) : socket(std::move(s)), parser(limits) {}

    net::Socket socket;
    resp::RequestParser parser;
    std::string out;          // encoded replies not yet written
    std::size_t out_pos = 0;  // how much of `out` has been written
    unsigned interest = net::readable;
    bool closing = false;  // close once `out` has been flushed (QUIT, protocol error)

    [[nodiscard]] std::size_t pending() const noexcept { return out.size() - out_pos; }
};

class Server::Reactor {
public:
    Reactor(const ServerConfig& config, CommandProcessor& processor, ServerCounters& counters)
        : config_(config), processor_(processor), counters_(counters) {
        std::tie(wake_rx_, wake_tx_) = net::make_wakeup_pair();
        poller_.add(wake_rx_.get(), net::readable);
    }
    ~Reactor() { stop(); }

    Reactor(const Reactor&) = delete;
    Reactor& operator=(const Reactor&) = delete;

    void start() {
        thread_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    void stop() noexcept {
        if (!thread_.joinable()) return;
        thread_.request_stop();
        wake(wake_tx_.get());
        thread_.join();
    }

    /// Called from the acceptor thread.
    void adopt(net::Socket client) {
        {
            std::scoped_lock lock(inbox_mutex_);
            inbox_.push_back(std::move(client));
        }
        wake(wake_tx_.get());
    }

private:
    using ConnectionMap = std::unordered_map<net::native_socket, std::unique_ptr<Connection>>;

    void run(std::stop_token stop) {
        std::vector<net::PollEvent> events;
        while (!stop.stop_requested()) {
            poller_.wait(events, -1ms);
            for (const net::PollEvent& ev : events) {
                if (ev.socket == wake_rx_.get()) {
                    drain(wake_rx_.get());
                    adopt_pending();
                    continue;
                }
                auto it = connections_.find(ev.socket);
                if (it == connections_.end()) continue;  // closed earlier in this batch
                Connection& c = *it->second;
                const bool alive = (ev.readable ? on_readable(c) : true) && (ev.writable ? pump(c) : true);
                if (!alive) close(it);
            }
        }
        counters_.connected_clients.fetch_sub(connections_.size(), std::memory_order_relaxed);
        connections_.clear();  // Socket destructors close everything
    }

    void adopt_pending() {
        std::vector<net::Socket> fresh;
        {
            std::scoped_lock lock(inbox_mutex_);
            fresh.swap(inbox_);
        }
        for (net::Socket& s : fresh) {
            const auto handle = s.get();
            poller_.add(handle, net::readable);
            connections_.emplace(handle, std::make_unique<Connection>(std::move(s), config_.limits));
        }
    }

    bool on_readable(Connection& c) {
        char buf[16 * 1024];
        // A bounded number of reads per wake-up so one firehose client can't
        // starve the others on this reactor (the poller is level-triggered).
        for (int i = 0; i < 16 && !c.closing && c.pending() <= config_.max_output_buffer; ++i) {
            const net::IoResult r = net::recv_some(c.socket.get(), buf, sizeof buf);
            if (r.status == net::IoResult::Status::would_block) break;
            if (r.status != net::IoResult::Status::ok) return false;  // peer closed or reset
            c.parser.feed({buf, r.bytes});
            process(c);
            if (r.bytes < sizeof buf) break;
        }
        return pump(c);
    }

    /// Executes every complete request in the buffer, appending replies.
    /// Stops early when the output buffer is over the limit (backpressure).
    void process(Connection& c) {
        try {
            while (!c.closing && c.pending() <= config_.max_output_buffer) {
                auto request = c.parser.next();
                if (!request) break;
                const bool quit = is_quit(request->front());
                resp::encode(processor_.execute(std::move(*request)), c.out);
                c.closing = quit;
            }
        } catch (const resp::ProtocolError& e) {
            resp::encode(resp::Reply::error(std::string("ERR ") + e.what()), c.out);
            c.closing = true;
        }
    }

    /// Writes as much as the socket accepts, resumes requests held back by
    /// backpressure, and keeps the poller interest in sync.
    /// Returns false when the connection should be closed.
    bool pump(Connection& c) {
        while (true) {
            while (c.pending() > 0) {
                const net::IoResult r = net::send_some(c.socket.get(), std::string_view(c.out).substr(c.out_pos));
                if (r.status == net::IoResult::Status::would_block) break;
                if (r.status != net::IoResult::Status::ok) return false;
                c.out_pos += r.bytes;
            }
            if (c.pending() > 0) break;  // kernel buffer full: wait for writable

            c.out.clear();
            c.out_pos = 0;
            if (c.out.capacity() > 1024 * 1024) c.out.shrink_to_fit();  // don't pin a burst's memory
            if (c.closing) return false;

            if (c.parser.buffered() == 0) break;
            const std::size_t before = c.out.size();
            process(c);  // requests that were waiting for the output to drain
            if (c.out.size() == before) break;  // only a partial request is buffered
        }

        unsigned want = c.pending() > 0 ? net::writable : 0u;
        if (!c.closing && c.pending() <= config_.max_output_buffer) want |= net::readable;
        if (want != c.interest) {
            poller_.modify(c.socket.get(), want);
            c.interest = want;
        }
        return true;
    }

    void close(ConnectionMap::iterator it) {
        poller_.remove(it->first);
        connections_.erase(it);
        counters_.connected_clients.fetch_sub(1, std::memory_order_relaxed);
    }

    const ServerConfig& config_;
    CommandProcessor& processor_;
    ServerCounters& counters_;

    net::Poller poller_;
    net::Socket wake_rx_;
    net::Socket wake_tx_;
    ConnectionMap connections_;  // touched only by this reactor's thread

    std::mutex inbox_mutex_;
    std::vector<net::Socket> inbox_;  // sockets handed over by the acceptor

    std::jthread thread_;  // last member: joined before the rest is destroyed
};

// ------------------------------------------------------------------- server

Server::Server(ServerConfig config, CommandProcessor& processor, ServerCounters& counters)
    : config_(std::move(config)), processor_(processor), counters_(counters) {}

Server::~Server() { stop(); }

void Server::start() {
    listener_ = net::listen_tcp(config_.bind, config_.port);
    port_ = net::local_port(listener_.get());
    std::tie(wake_rx_, wake_tx_) = net::make_wakeup_pair();

    const std::size_t threads =
        config_.io_threads ? config_.io_threads : std::max<std::size_t>(1, std::thread::hardware_concurrency());
    reactors_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
        reactors_.push_back(std::make_unique<Reactor>(config_, processor_, counters_));
        reactors_.back()->start();
    }
    acceptor_ = std::jthread([this](std::stop_token stop) { accept_loop(stop); });
}

void Server::stop() noexcept {
    if (acceptor_.joinable()) {
        acceptor_.request_stop();
        wake(wake_tx_.get());
        acceptor_.join();
    }
    for (auto& reactor : reactors_) reactor->stop();
    reactors_.clear();
    listener_.reset();
}

void Server::accept_loop(std::stop_token stop) {
    net::Poller poller;
    poller.add(listener_.get(), net::readable);
    poller.add(wake_rx_.get(), net::readable);
    std::vector<net::PollEvent> events;
    while (!stop.stop_requested()) {
        poller.wait(events, -1ms);
        for (const auto& ev : events)
            if (ev.socket == listener_.get()) accept_pending();
    }
}

void Server::accept_pending() {
    while (true) {
        net::Socket client;
        try {
            client = net::accept_client(listener_.get());
        } catch (const std::system_error& e) {
            // Typically EMFILE (out of file descriptors). Back off briefly
            // instead of spinning on a listener that stays readable.
            std::fprintf(stderr, "emberkv: accept failed: %s\n", e.what());
            std::this_thread::sleep_for(10ms);
            return;
        }
        if (!client) return;  // nothing more pending

        counters_.total_connections.fetch_add(1, std::memory_order_relaxed);
        if (counters_.connected_clients.load(std::memory_order_relaxed) >= config_.max_clients) {
            counters_.rejected_connections.fetch_add(1, std::memory_order_relaxed);
            (void)net::send_some(client.get(), "-ERR max number of clients reached\r\n");
            continue;  // the Socket destructor closes it
        }
        net::set_nonblocking(client.get());
        net::set_nodelay(client.get());
        counters_.connected_clients.fetch_add(1, std::memory_order_relaxed);
        reactors_[next_reactor_++ % reactors_.size()]->adopt(std::move(client));
    }
}

}  // namespace emberkv
