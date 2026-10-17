#pragma once

#include "emberkv/server.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace emberkv {

struct Config {
    ServerConfig server;
    std::size_t shards = 64;
    std::size_t max_memory = 256u * 1024 * 1024;
    std::optional<std::filesystem::path> snapshot;  // loaded at startup, saved at shutdown
    bool help = false;
    bool version = false;
};

/// Parses command-line arguments (without the program name).
/// Throws std::invalid_argument with a user-facing message on bad input.
[[nodiscard]] Config parse_args(std::span<const std::string_view> args);

/// "1024", "512kb", "64mb", "2gb" (case-insensitive, optional 'b') -> bytes.
[[nodiscard]] std::optional<std::size_t> parse_size(std::string_view text) noexcept;

[[nodiscard]] std::string usage(std::string_view program);

}  // namespace emberkv
