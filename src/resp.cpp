#include "emberkv/resp.hpp"

#include "emberkv/value.hpp"  // overloaded

#include <algorithm>
#include <charconv>
#include <limits>

namespace emberkv::resp {

namespace {

void append_int(std::string& out, std::int64_t n) {
    char buf[24];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, n);
    out.append(buf, end);
}

void append_header(std::string& out, char type, std::int64_t n) {
    out.push_back(type);
    append_int(out, n);
    out += "\r\n";
}

/// Parses a whole string_view as a signed decimal integer.
std::optional<std::int64_t> parse_int(std::string_view s) {
    std::int64_t value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || ptr != s.data() + s.size() || s.empty()) return std::nullopt;
    return value;
}

/// Simple strings and errors can't contain CR/LF; replace rather than corrupt the stream.
void append_line(std::string& out, std::string_view text) {
    for (char c : text) out.push_back(c == '\r' || c == '\n' ? ' ' : c);
    out += "\r\n";
}

}  // namespace

void encode(const Reply& reply, std::string& out) {
    std::visit(overloaded{
                   [&](const SimpleString& s) {
                       out.push_back('+');
                       append_line(out, s.value);
                   },
                   [&](const Error& e) {
                       out.push_back('-');
                       append_line(out, e.message);
                   },
                   [&](std::int64_t n) { append_header(out, ':', n); },
                   [&](const std::string& bulk) {
                       append_header(out, '$', static_cast<std::int64_t>(bulk.size()));
                       out += bulk;
                       out += "\r\n";
                   },
                   [&](const Null&) { out += "$-1\r\n"; },
                   [&](const Array& items) {
                       append_header(out, '*', static_cast<std::int64_t>(items.size()));
                       for (const Reply& item : items) encode(item, out);
                   },
               },
               reply.value);
}

std::string encode(const Reply& reply) {
    std::string out;
    encode(reply, out);
    return out;
}

std::string encode_command(std::span<const std::string> args) {
    std::string out;
    append_header(out, '*', static_cast<std::int64_t>(args.size()));
    for (const auto& arg : args) {
        append_header(out, '$', static_cast<std::int64_t>(arg.size()));
        out += arg;
        out += "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------- requests

void RequestParser::feed(std::string_view bytes) {
    compact();
    buffer_.append(bytes);
}

void RequestParser::compact() {
    // Drop consumed bytes once they dominate the buffer. Amortised O(1) per byte.
    if (pos_ > 0 && (pos_ == buffer_.size() || pos_ >= 4096)) {
        buffer_.erase(0, pos_);
        pos_ = 0;
    }
}

std::optional<std::string_view> RequestParser::read_line() {
    const std::string_view rest = std::string_view{buffer_}.substr(pos_);
    const auto crlf = rest.find("\r\n");
    if (crlf == std::string_view::npos) {
        // A header line is a few bytes; an endless one is an attack or garbage.
        if (rest.size() > limits_.max_inline_length) throw ProtocolError("Protocol error: too big inline request");
        return std::nullopt;
    }
    pos_ += crlf + 2;
    return rest.substr(0, crlf);
}

std::optional<std::vector<std::string>> RequestParser::next() {
    while (true) {
        if (!in_multibulk_) {
            if (pos_ >= buffer_.size()) return std::nullopt;
            if (buffer_[pos_] != '*') {
                auto args = parse_inline();
                if (!args) return std::nullopt;
                if (args->empty()) continue;  // blank line: skip it
                return args;
            }

            auto line = read_line();
            if (!line) return std::nullopt;
            auto count = parse_int(line->substr(1));
            if (!count) throw ProtocolError("Protocol error: invalid multibulk length");
            if (*count <= 0) continue;  // "*0" and "*-1" are valid no-ops
            if (static_cast<std::uint64_t>(*count) > limits_.max_arguments)
                throw ProtocolError("Protocol error: invalid multibulk length");

            in_multibulk_ = true;
            args_remaining_ = *count;
            bulk_length_ = -1;
            args_.clear();
            args_.reserve(static_cast<std::size_t>(std::min<std::int64_t>(*count, 1024)));
        }

        while (args_remaining_ > 0) {
            if (bulk_length_ < 0) {
                if (pos_ >= buffer_.size()) return std::nullopt;
                if (buffer_[pos_] != '$')
                    throw ProtocolError(std::string("Protocol error: expected '$', got '") + buffer_[pos_] + "'");
                auto line = read_line();
                if (!line) return std::nullopt;
                auto len = parse_int(line->substr(1));
                if (!len || *len < 0 || static_cast<std::uint64_t>(*len) > limits_.max_bulk_length)
                    throw ProtocolError("Protocol error: invalid bulk length");
                bulk_length_ = *len;
            }

            const auto len = static_cast<std::size_t>(bulk_length_);
            if (buffer_.size() - pos_ < len + 2) {
                // Pre-size for a large value so it arrives without repeated regrowth.
                if (buffer_.capacity() < pos_ + len + 2) buffer_.reserve(pos_ + len + 2);
                return std::nullopt;
            }
            if (buffer_[pos_ + len] != '\r' || buffer_[pos_ + len + 1] != '\n')
                throw ProtocolError("Protocol error: bulk string not terminated by CRLF");

            args_.emplace_back(buffer_, pos_, len);
            pos_ += len + 2;
            bulk_length_ = -1;
            --args_remaining_;
        }

        in_multibulk_ = false;
        return std::move(args_);
    }
}

std::optional<std::vector<std::string>> RequestParser::parse_inline() {
    auto line = read_line();
    if (!line) return std::nullopt;

    std::vector<std::string> args;
    std::string_view rest = *line;
    while (true) {
        const auto start = rest.find_first_not_of(" \t");
        if (start == std::string_view::npos) break;
        rest.remove_prefix(start);
        const auto end = rest.find_first_of(" \t");
        args.emplace_back(rest.substr(0, end));
        if (end == std::string_view::npos) break;
        rest.remove_prefix(end);
    }
    return args;
}

// ---------------------------------------------------------------- replies

namespace {

struct ReplyReader {
    std::string_view in;
    std::size_t pos = 0;

    std::optional<std::string_view> line() {
        const auto crlf = in.find("\r\n", pos);
        if (crlf == std::string_view::npos) return std::nullopt;
        auto result = in.substr(pos, crlf - pos);
        pos = crlf + 2;
        return result;
    }

    std::optional<Reply> read(int depth = 0) {
        if (depth > 64) throw ProtocolError("reply nested too deeply");
        if (pos >= in.size()) return std::nullopt;
        const char type = in[pos++];
        auto body = line();
        if (!body) return std::nullopt;

        switch (type) {
            case '+': return Reply::simple(std::string(*body));
            case '-': return Reply::error(std::string(*body));
            case ':': {
                auto n = parse_int(*body);
                if (!n) throw ProtocolError("bad integer reply");
                return Reply::integer(*n);
            }
            case '$': {
                auto len = parse_int(*body);
                if (!len || *len < -1) throw ProtocolError("bad bulk length");
                if (*len == -1) return Reply::null();
                const auto n = static_cast<std::size_t>(*len);
                if (in.size() - pos < n + 2) return std::nullopt;
                Reply r = Reply::bulk(std::string(in.substr(pos, n)));
                pos += n + 2;
                return r;
            }
            case '*': {
                auto count = parse_int(*body);
                if (!count || *count < -1) throw ProtocolError("bad array length");
                if (*count == -1) return Reply::null();
                Array items;
                items.reserve(static_cast<std::size_t>(std::min<std::int64_t>(*count, 1024)));
                for (std::int64_t i = 0; i < *count; ++i) {
                    auto item = read(depth + 1);
                    if (!item) return std::nullopt;
                    items.push_back(std::move(*item));
                }
                return Reply::array(std::move(items));
            }
            default: throw ProtocolError(std::string("unknown reply type '") + type + "'");
        }
    }
};

}  // namespace

std::optional<std::pair<Reply, std::size_t>> parse_reply(std::string_view input) {
    ReplyReader reader{input};
    auto reply = reader.read();
    if (!reply) return std::nullopt;
    return std::pair{std::move(*reply), reader.pos};
}

}  // namespace emberkv::resp
