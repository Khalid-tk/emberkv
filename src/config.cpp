#include "emberkv/config.hpp"

#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <stdexcept>

namespace emberkv {

namespace {

template <class T>
std::optional<T> parse_number(std::string_view s) noexcept {
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

}  // namespace

std::optional<std::size_t> parse_size(std::string_view text) noexcept {
    std::size_t digits = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) ++digits;
    const auto number = parse_number<std::size_t>(text.substr(0, digits));
    if (!number) return std::nullopt;

    std::string suffix;
    for (char c : text.substr(digits)) suffix.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (!suffix.empty() && suffix.back() == 'b') suffix.pop_back();

    std::size_t multiplier = 1;
    if (suffix == "k") multiplier = std::size_t{1} << 10;
    else if (suffix == "m") multiplier = std::size_t{1} << 20;
    else if (suffix == "g") multiplier = std::size_t{1} << 30;
    else if (!suffix.empty()) return std::nullopt;

    if (*number > std::numeric_limits<std::size_t>::max() / multiplier) return std::nullopt;
    return *number * multiplier;
}

Config parse_args(std::span<const std::string_view> args) {
    Config config;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view flag = args[i];
        auto value = [&]() -> std::string_view {
            if (i + 1 >= args.size()) throw std::invalid_argument(std::format("{} needs a value", flag));
            return args[++i];
        };
        auto invalid = [&](std::string_view v) {
            return std::invalid_argument(std::format("invalid value '{}' for {}", v, flag));
        };

        if (flag == "-h" || flag == "--help") {
            config.help = true;
        } else if (flag == "-v" || flag == "--version") {
            config.version = true;
        } else if (flag == "--bind") {
            config.server.bind = std::string(value());
        } else if (flag == "-p" || flag == "--port") {
            const auto v = value();
            const auto port = parse_number<std::uint16_t>(v);
            if (!port) throw invalid(v);
            config.server.port = *port;
        } else if (flag == "--threads") {
            const auto v = value();
            const auto n = parse_number<std::size_t>(v);
            if (!n || *n == 0 || *n > 1024) throw invalid(v);
            config.server.io_threads = *n;
        } else if (flag == "--shards") {
            const auto v = value();
            const auto n = parse_number<std::size_t>(v);
            if (!n || *n == 0 || *n > 65536) throw invalid(v);
            config.shards = *n;
        } else if (flag == "--maxmemory") {
            const auto v = value();
            const auto n = parse_size(v);
            if (!n || *n < 1024 * 1024) throw std::invalid_argument("--maxmemory must be at least 1mb");
            config.max_memory = *n;
        } else if (flag == "--maxclients") {
            const auto v = value();
            const auto n = parse_number<std::size_t>(v);
            if (!n || *n == 0) throw invalid(v);
            config.server.max_clients = *n;
        } else if (flag == "--snapshot") {
            config.snapshot = std::filesystem::path(std::string(value()));
        } else {
            throw std::invalid_argument(std::format("unknown option '{}' (try --help)", flag));
        }
    }
    return config;
}

std::string usage(std::string_view program) {
    return std::format(
        "Usage: {} [options]\n"
        "\n"
        "  --bind <address>      address to listen on            (default 127.0.0.1)\n"
        "  -p, --port <port>     TCP port, 0 for any free port    (default 6380)\n"
        "  --threads <n>         I/O threads                      (default: CPU count)\n"
        "  --shards <n>          lock shards, rounded up to 2^k   (default 64)\n"
        "  --maxmemory <size>    memory budget, e.g. 512mb, 2gb   (default 256mb)\n"
        "  --maxclients <n>      connection limit                 (default 10000)\n"
        "  --snapshot <file>     load at startup, SAVE/BGSAVE target, saved on shutdown\n"
        "  -v, --version         print the version\n"
        "  -h, --help            print this help\n",
        program);
}

}  // namespace emberkv
