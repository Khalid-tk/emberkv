#pragma once

// A thin portability layer over BSD sockets and Winsock. Everything above this
// header is platform-independent; everything platform-specific lives in
// src/net.cpp behind these declarations.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace emberkv::net {

#ifdef _WIN32
using native_socket = SOCKET;
inline constexpr native_socket invalid_socket = INVALID_SOCKET;
#else
using native_socket = int;
inline constexpr native_socket invalid_socket = -1;
#endif

/// Owns one socket handle. Move-only: exactly one owner closes it, exactly once.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(native_socket handle) noexcept : handle_(handle) {}
    ~Socket() { reset(); }

    Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_, invalid_socket)) {}
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) reset(std::exchange(other.handle_, invalid_socket));
        return *this;
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    [[nodiscard]] native_socket get() const noexcept { return handle_; }
    [[nodiscard]] native_socket release() noexcept { return std::exchange(handle_, invalid_socket); }
    explicit operator bool() const noexcept { return handle_ != invalid_socket; }
    void reset(native_socket handle = invalid_socket) noexcept;

private:
    native_socket handle_ = invalid_socket;
};

/// Process-wide network setup: WSAStartup on Windows, ignoring SIGPIPE on
/// POSIX (a write to a closed peer must be an error code, not a dead process).
/// Create one in main() before any socket call.
class NetworkInit {
public:
    NetworkInit();
    ~NetworkInit();
    NetworkInit(const NetworkInit&) = delete;
    NetworkInit& operator=(const NetworkInit&) = delete;
};

/// The socket error category for the current platform (errno or WSAGetLastError).
[[nodiscard]] std::error_code last_error() noexcept;
[[nodiscard]] bool would_block(const std::error_code& ec) noexcept;

/// Result of a non-blocking read or write.
struct IoResult {
    std::size_t bytes = 0;
    enum class Status { ok, would_block, closed, error } status = Status::ok;
    std::error_code error;
};

[[nodiscard]] IoResult send_some(native_socket s, std::string_view data) noexcept;
[[nodiscard]] IoResult recv_some(native_socket s, char* buffer, std::size_t capacity) noexcept;

/// Blocking helpers for clients and tests; throw std::system_error on failure.
void send_all(native_socket s, std::string_view data);

void set_nonblocking(native_socket s);
void set_nodelay(native_socket s) noexcept;

/// Binds and listens. Port 0 picks a free port; read it back with local_port().
[[nodiscard]] Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog = 511);
[[nodiscard]] Socket connect_tcp(const std::string& host, std::uint16_t port);
[[nodiscard]] std::uint16_t local_port(native_socket s);

/// Non-blocking accept: an empty Socket means "no pending connection".
[[nodiscard]] Socket accept_client(native_socket listener);

/// A connected pair used to wake a thread blocked in Poller::wait():
/// socketpair() on POSIX, a loopback TCP pair on Windows (WSAPoll can only
/// wait on sockets, so a pipe would not work there).
[[nodiscard]] std::pair<Socket, Socket> make_wakeup_pair();

enum Interest : unsigned { readable = 1u << 0, writable = 1u << 1 };

struct PollEvent {
    native_socket socket;
    bool readable;
    bool writable;
};

/// Readiness notification. epoll on Linux; poll()/WSAPoll() elsewhere (or on
/// Linux with -DEMBERKV_FORCE_POLL=ON). The backend is hidden behind a pimpl
/// so platform headers never leak into the rest of the code base.
class Poller {
public:
    Poller();
    ~Poller();
    Poller(Poller&&) noexcept;
    Poller& operator=(Poller&&) noexcept;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void add(native_socket s, unsigned interest);
    void modify(native_socket s, unsigned interest);
    void remove(native_socket s) noexcept;

    /// Waits up to `timeout` (negative: forever). Hang-ups and errors are
    /// reported as readable, so the next recv() surfaces them.
    std::size_t wait(std::vector<PollEvent>& events, std::chrono::milliseconds timeout);

    [[nodiscard]] static std::string_view backend() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace emberkv::net
