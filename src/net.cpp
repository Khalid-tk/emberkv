#include "emberkv/net.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#if defined(__linux__) && !defined(EMBERKV_FORCE_POLL)
#define EMBERKV_USE_EPOLL 1
#include <sys/epoll.h>
#endif
#endif

namespace emberkv::net {

// ------------------------------------------------------------------- basics

void Socket::reset(native_socket handle) noexcept {
    if (handle_ != invalid_socket) {
#ifdef _WIN32
        ::closesocket(handle_);
#else
        ::close(handle_);
#endif
    }
    handle_ = handle;
}

NetworkInit::NetworkInit() {
#ifdef _WIN32
    WSADATA data;
    if (const int rc = ::WSAStartup(MAKEWORD(2, 2), &data); rc != 0)
        throw std::system_error(rc, std::system_category(), "WSAStartup");
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

NetworkInit::~NetworkInit() {
#ifdef _WIN32
    ::WSACleanup();
#endif
}

std::error_code last_error() noexcept {
#ifdef _WIN32
    return {::WSAGetLastError(), std::system_category()};
#else
    return {errno, std::system_category()};
#endif
}

bool would_block(const std::error_code& ec) noexcept {
#ifdef _WIN32
    return ec.value() == WSAEWOULDBLOCK || ec.value() == WSAEINTR;
#else
    return ec.value() == EAGAIN || ec.value() == EWOULDBLOCK || ec.value() == EINTR;
#endif
}

namespace {

[[noreturn]] void throw_last(const char* what) { throw std::system_error(last_error(), what); }

IoResult classify_failure() noexcept {
    const auto ec = last_error();
    if (would_block(ec)) return {0, IoResult::Status::would_block, {}};
    return {0, IoResult::Status::error, ec};
}

struct AddrInfoDeleter {
    void operator()(addrinfo* p) const noexcept { ::freeaddrinfo(p); }
};
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

AddrInfoPtr resolve(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* raw = nullptr;
    const std::string service = std::to_string(port);
    if (const int rc = ::getaddrinfo(host.empty() ? nullptr : host.c_str(), service.c_str(), &hints, &raw); rc != 0)
#ifdef _WIN32
        // gai_strerror is a wide-char macro when UNICODE is defined; ask for the narrow one.
        throw std::runtime_error("cannot resolve '" + host + "': " + ::gai_strerrorA(rc));
#else
        throw std::runtime_error("cannot resolve '" + host + "': " + ::gai_strerror(rc));
#endif
    return AddrInfoPtr(raw);
}

Socket open_socket(const addrinfo& ai) {
    return Socket(::socket(ai.ai_family, ai.ai_socktype, ai.ai_protocol));
}

// bind/connect take an int on Windows and a socklen_t on POSIX.
auto address_length(const addrinfo& ai) noexcept {
#ifdef _WIN32
    return static_cast<int>(ai.ai_addrlen);
#else
    return ai.ai_addrlen;
#endif
}

}  // namespace

IoResult send_some(native_socket s, std::string_view data) noexcept {
#ifdef _WIN32
    const int len = static_cast<int>(std::min<std::size_t>(data.size(), INT_MAX));
    const int n = ::send(s, data.data(), len, 0);
    if (n == SOCKET_ERROR) return classify_failure();
#else
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;  // SIGPIPE is ignored process-wide instead
#endif
    const ssize_t n = ::send(s, data.data(), data.size(), flags);
    if (n < 0) return classify_failure();
#endif
    return {static_cast<std::size_t>(n), IoResult::Status::ok, {}};
}

IoResult recv_some(native_socket s, char* buffer, std::size_t capacity) noexcept {
#ifdef _WIN32
    const int n = ::recv(s, buffer, static_cast<int>(std::min<std::size_t>(capacity, INT_MAX)), 0);
    if (n == SOCKET_ERROR) return classify_failure();
#else
    const ssize_t n = ::recv(s, buffer, capacity, 0);
    if (n < 0) return classify_failure();
#endif
    if (n == 0) return {0, IoResult::Status::closed, {}};
    return {static_cast<std::size_t>(n), IoResult::Status::ok, {}};
}

void send_all(native_socket s, std::string_view data) {
    while (!data.empty()) {
        const IoResult r = send_some(s, data);
        if (r.status == IoResult::Status::error) throw std::system_error(r.error, "send");
        data.remove_prefix(r.bytes);
    }
}

void set_nonblocking(native_socket s) {
#ifdef _WIN32
    u_long enabled = 1;
    if (::ioctlsocket(s, static_cast<long>(FIONBIO), &enabled) != 0) throw_last("ioctlsocket(FIONBIO)");
#else
    const int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0 || ::fcntl(s, F_SETFL, flags | O_NONBLOCK) < 0) throw_last("fcntl(O_NONBLOCK)");
#endif
}

void set_nodelay(native_socket s) noexcept {
    // Disable Nagle: replies are small and latency matters more than packet count.
    const int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
    const AddrInfoPtr list = resolve(host, port, true);
    std::error_code last;
    for (const addrinfo* ai = list.get(); ai; ai = ai->ai_next) {
        Socket s = open_socket(*ai);
        if (!s) {
            last = net::last_error();
            continue;
        }
        const int one = 1;
#ifdef _WIN32
        // SO_REUSEADDR on Windows lets another process steal the port.
        ::setsockopt(s.get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
        ::setsockopt(s.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        if (::bind(s.get(), ai->ai_addr, address_length(*ai)) != 0 || ::listen(s.get(), backlog) != 0) {
            last = net::last_error();
            continue;
        }
        set_nonblocking(s.get());
        return s;
    }
    throw std::system_error(last, "cannot listen on " + host + ":" + std::to_string(port));
}

Socket connect_tcp(const std::string& host, std::uint16_t port) {
    const AddrInfoPtr list = resolve(host, port, false);
    std::error_code last;
    for (const addrinfo* ai = list.get(); ai; ai = ai->ai_next) {
        Socket s = open_socket(*ai);
        if (s && ::connect(s.get(), ai->ai_addr, address_length(*ai)) == 0) {
            set_nodelay(s.get());
            return s;
        }
        last = net::last_error();
    }
    throw std::system_error(last, "cannot connect to " + host + ":" + std::to_string(port));
}

std::uint16_t local_port(native_socket s) {
    sockaddr_storage addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) != 0) throw_last("getsockname");
    if (addr.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
}

Socket accept_client(native_socket listener) {
    Socket client(::accept(listener, nullptr, nullptr));
    if (client) return client;
    const auto ec = last_error();
#ifdef _WIN32
    if (would_block(ec) || ec.value() == WSAECONNRESET) return {};
#else
    if (would_block(ec) || ec.value() == ECONNABORTED) return {};
#endif
    throw std::system_error(ec, "accept");
}

std::pair<Socket, Socket> make_wakeup_pair() {
#ifdef _WIN32
    Socket listener = listen_tcp("127.0.0.1", 0, 1);
    Socket writer = connect_tcp("127.0.0.1", local_port(listener.get()));
    Socket reader;
    // The listener is non-blocking; the connection is already queued, but give
    // the stack a moment if it hasn't surfaced yet.
    for (int attempt = 0; attempt < 1000 && !reader; ++attempt) {
        reader = accept_client(listener.get());
        if (!reader) ::Sleep(1);
    }
    if (!reader) throw std::runtime_error("cannot create wake-up socket pair");
#else
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) throw_last("socketpair");
    Socket reader(fds[0]);
    Socket writer(fds[1]);
#endif
    set_nonblocking(reader.get());
    set_nonblocking(writer.get());
    return {std::move(reader), std::move(writer)};
}

// ------------------------------------------------------------------- poller

#ifdef EMBERKV_USE_EPOLL

struct Poller::Impl {
    int epfd = ::epoll_create1(EPOLL_CLOEXEC);
    std::vector<epoll_event> buffer = std::vector<epoll_event>(256);

    Impl() {
        if (epfd < 0) throw_last("epoll_create1");
    }
    ~Impl() { ::close(epfd); }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static std::uint32_t mask(unsigned interest) noexcept {
        std::uint32_t ev = EPOLLRDHUP;
        if (interest & readable) ev |= EPOLLIN;
        if (interest & writable) ev |= EPOLLOUT;
        return ev;
    }
    void control(int op, native_socket s, unsigned interest) {
        epoll_event ev{};
        ev.events = mask(interest);
        ev.data.fd = s;
        if (::epoll_ctl(epfd, op, s, &ev) != 0) throw_last("epoll_ctl");
    }
};

void Poller::add(native_socket s, unsigned interest) { impl_->control(EPOLL_CTL_ADD, s, interest); }
void Poller::modify(native_socket s, unsigned interest) { impl_->control(EPOLL_CTL_MOD, s, interest); }
void Poller::remove(native_socket s) noexcept { ::epoll_ctl(impl_->epfd, EPOLL_CTL_DEL, s, nullptr); }

std::size_t Poller::wait(std::vector<PollEvent>& events, std::chrono::milliseconds timeout) {
    events.clear();
    const int n = ::epoll_wait(impl_->epfd, impl_->buffer.data(), static_cast<int>(impl_->buffer.size()),
                               timeout.count() < 0 ? -1 : static_cast<int>(timeout.count()));
    if (n < 0) {
        if (errno == EINTR) return 0;
        throw_last("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
        const epoll_event& e = impl_->buffer[static_cast<std::size_t>(i)];
        events.push_back({e.data.fd, (e.events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0,
                          (e.events & EPOLLOUT) != 0});
    }
    if (static_cast<std::size_t>(n) == impl_->buffer.size()) impl_->buffer.resize(impl_->buffer.size() * 2);
    return events.size();
}

std::string_view Poller::backend() noexcept { return "epoll"; }

#else  // poll() / WSAPoll()

#ifdef _WIN32
using pollfd_type = WSAPOLLFD;
#else
using pollfd_type = pollfd;
#endif

struct Poller::Impl {
    std::vector<pollfd_type> fds;
    std::unordered_map<native_socket, std::size_t> index;  // socket -> slot in fds

    static short mask(unsigned interest) noexcept {
        short ev = 0;
        if (interest & readable) ev = static_cast<short>(ev | POLLIN);
        if (interest & writable) ev = static_cast<short>(ev | POLLOUT);
        return ev;
    }
};

void Poller::add(native_socket s, unsigned interest) {
    impl_->index.emplace(s, impl_->fds.size());
    pollfd_type p{};
    p.fd = s;
    p.events = Impl::mask(interest);
    impl_->fds.push_back(p);
}

void Poller::modify(native_socket s, unsigned interest) { impl_->fds[impl_->index.at(s)].events = Impl::mask(interest); }

void Poller::remove(native_socket s) noexcept {
    auto it = impl_->index.find(s);
    if (it == impl_->index.end()) return;
    // O(1) removal: move the last slot into the hole.
    const std::size_t slot = it->second;
    impl_->fds[slot] = impl_->fds.back();
    impl_->index[impl_->fds[slot].fd] = slot;
    impl_->fds.pop_back();
    impl_->index.erase(s);
}

std::size_t Poller::wait(std::vector<PollEvent>& events, std::chrono::milliseconds timeout) {
    events.clear();
    const int ms = timeout.count() < 0 ? -1 : static_cast<int>(timeout.count());
#ifdef _WIN32
    const int n = ::WSAPoll(impl_->fds.data(), static_cast<ULONG>(impl_->fds.size()), ms);
#else
    const int n = ::poll(impl_->fds.data(), static_cast<nfds_t>(impl_->fds.size()), ms);
#endif
    if (n < 0) {
        if (would_block(last_error())) return 0;
        throw_last("poll");
    }
    for (const pollfd_type& p : impl_->fds) {
        if (p.revents == 0) continue;
        events.push_back({p.fd, (p.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0, (p.revents & POLLOUT) != 0});
    }
    return events.size();
}

std::string_view Poller::backend() noexcept {
#ifdef _WIN32
    return "WSAPoll";
#else
    return "poll";
#endif
}

#endif

Poller::Poller() : impl_(std::make_unique<Impl>()) {}
Poller::~Poller() = default;
Poller::Poller(Poller&&) noexcept = default;
Poller& Poller::operator=(Poller&&) noexcept = default;

}  // namespace emberkv::net
