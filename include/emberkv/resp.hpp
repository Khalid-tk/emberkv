#pragma once

// RESP2, the Redis wire protocol. Speaking it means stock tools work against
// EmberKV unchanged: redis-cli, redis-benchmark and every Redis client library.
//
//   +OK\r\n                 simple string
//   -ERR message\r\n        error
//   :42\r\n                 integer
//   $5\r\nhello\r\n         bulk string        ($-1\r\n is null)
//   *2\r\n$3\r\nGET\r\n...  array              (*-1\r\n is null)
//
// Clients send commands as arrays of bulk strings, or as a plain text line
// ("inline commands") which is what you get when typing into telnet or nc.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace emberkv::resp {

struct SimpleString {
    std::string value;
    friend bool operator==(const SimpleString&, const SimpleString&) = default;
};
struct Error {
    std::string message;
    friend bool operator==(const Error&, const Error&) = default;
};
struct Null {
    friend bool operator==(const Null&, const Null&) = default;
};

struct Reply;
using Array = std::vector<Reply>;  // vector of an incomplete type is fine since C++17

/// Any value the server can send back. Bulk strings are plain std::string.
struct Reply {
    std::variant<SimpleString, Error, std::int64_t, std::string, Null, Array> value;

    static Reply ok() { return {SimpleString{"OK"}}; }
    static Reply simple(std::string s) { return {SimpleString{std::move(s)}}; }
    static Reply error(std::string message) { return {Error{std::move(message)}}; }
    static Reply integer(std::int64_t n) { return {n}; }
    static Reply bulk(std::string s) { return {std::move(s)}; }
    static Reply null() { return {Null{}}; }
    static Reply array(Array items) { return {std::move(items)}; }

    [[nodiscard]] bool is_error() const noexcept { return std::holds_alternative<Error>(value); }

    friend bool operator==(const Reply&, const Reply&) = default;
};

/// Appends the wire encoding of `reply` to `out` (no intermediate strings).
void encode(const Reply& reply, std::string& out);
[[nodiscard]] std::string encode(const Reply& reply);

/// Encodes a command the way clients send it: an array of bulk strings.
[[nodiscard]] std::string encode_command(std::span<const std::string> args);

/// Malformed input. The connection cannot be resynchronised after this, so
/// the server replies with the message and closes it.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Incremental request parser. Bytes arrive in arbitrary chunks (a request can
/// be split across reads, and one read can hold many pipelined requests), so
/// the parser keeps its progress between calls instead of re-scanning.
class RequestParser {
public:
    struct Limits {
        std::size_t max_bulk_length = 512u * 1024 * 1024;
        std::size_t max_arguments = 1024u * 1024;
        std::size_t max_inline_length = 64u * 1024;
    };

    RequestParser() : RequestParser(Limits{}) {}
    explicit RequestParser(Limits limits) : limits_(limits) {}

    void feed(std::string_view bytes);

    /// The next complete request, or std::nullopt if more bytes are needed.
    /// Throws ProtocolError on malformed input.
    [[nodiscard]] std::optional<std::vector<std::string>> next();

    /// Bytes received but not yet consumed by a complete request.
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - pos_; }

private:
    [[nodiscard]] std::optional<std::string_view> read_line();
    [[nodiscard]] std::optional<std::vector<std::string>> parse_inline();
    void compact();

    Limits limits_;
    std::string buffer_;
    std::size_t pos_ = 0;

    // Progress through a multi-bulk request that is not complete yet.
    bool in_multibulk_ = false;
    std::int64_t args_remaining_ = 0;
    std::int64_t bulk_length_ = -1;  // -1: next thing to read is a $<len> header
    std::vector<std::string> args_;
};

/// Parses one reply from the start of `input` (used by the CLI and tests).
/// Returns the reply and the number of bytes it occupied, or std::nullopt if
/// `input` holds only part of a reply. Throws ProtocolError on bad input.
[[nodiscard]] std::optional<std::pair<Reply, std::size_t>> parse_reply(std::string_view input);

}  // namespace emberkv::resp
