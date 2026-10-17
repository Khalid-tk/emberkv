#pragma once

#include "emberkv/commands.hpp"
#include "emberkv/net.hpp"
#include "emberkv/resp.hpp"
#include "emberkv/store.hpp"

#include <atomic>
#include <chrono>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace emberkv::resp {
// Lets GoogleTest print a Reply readably when an assertion fails.
inline void PrintTo(const Reply& reply, std::ostream* os) {
    std::string wire = encode(reply);
    std::string shown;
    for (char c : wire) shown += c == '\r' ? std::string("\\r") : c == '\n' ? std::string("\\n") : std::string(1, c);
    *os << shown;
}
}  // namespace emberkv::resp

namespace emberkv::testing {

using namespace std::chrono_literals;

/// A clock the test moves by hand, so TTL tests never sleep or flake.
class ManualClock {
public:
    [[nodiscard]] Store::Clock function() const {
        return [ms = ms_] { return TimePoint{} + std::chrono::hours{1} + std::chrono::milliseconds{ms->load()}; };
    }
    void advance(std::chrono::milliseconds d) { ms_->fetch_add(d.count()); }

private:
    std::shared_ptr<std::atomic<std::int64_t>> ms_ = std::make_shared<std::atomic<std::int64_t>>(0);
};

inline resp::Reply bulk(std::string s) { return resp::Reply::bulk(std::move(s)); }
inline resp::Reply integer(std::int64_t n) { return resp::Reply::integer(n); }
inline resp::Reply null() { return resp::Reply::null(); }
inline resp::Reply ok() { return resp::Reply::ok(); }
inline resp::Reply array(std::initializer_list<resp::Reply> items) { return resp::Reply::array(items); }

/// True if the reply is an error whose message starts with `prefix`.
inline bool is_error(const resp::Reply& r, std::string_view prefix = "") {
    const auto* e = std::get_if<resp::Error>(&r.value);
    return e && e->message.starts_with(prefix);
}

/// A blocking RESP client for integration tests.
class TestClient {
public:
    explicit TestClient(std::uint16_t port) : socket_(net::connect_tcp("127.0.0.1", port)) {}

    void send_raw(std::string_view bytes) { net::send_all(socket_.get(), bytes); }

    resp::Reply call(std::vector<std::string> args) {
        send_raw(resp::encode_command(args));
        return read();
    }

    resp::Reply read() {
        while (true) {
            if (auto parsed = resp::parse_reply(buffer_)) {
                buffer_.erase(0, parsed->second);
                return std::move(parsed->first);
            }
            if (!fill()) throw std::runtime_error("connection closed before a full reply arrived");
        }
    }

    /// True once the server has closed the connection (and no bytes remain).
    bool closed_by_server() {
        while (buffer_.empty()) {
            if (!fill()) return true;
        }
        return false;
    }

    net::native_socket handle() const { return socket_.get(); }

private:
    bool fill() {
        char chunk[64 * 1024];
        const auto r = net::recv_some(socket_.get(), chunk, sizeof chunk);
        if (r.status != net::IoResult::Status::ok) return false;
        buffer_.append(chunk, r.bytes);
        return true;
    }

    net::Socket socket_;
    std::string buffer_;
};

}  // namespace emberkv::testing
