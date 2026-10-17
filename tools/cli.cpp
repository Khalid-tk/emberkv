// emberkv-cli: a small redis-cli-style client that reuses the server's RESP
// code. Works on Windows, where redis-cli usually isn't installed.
//
//   emberkv-cli                     interactive prompt
//   emberkv-cli SET greeting hello  one-shot command

#include "emberkv/net.hpp"
#include "emberkv/resp.hpp"
#include "emberkv/value.hpp"  // overloaded

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using emberkv::resp::Reply;

namespace {

class Client {
public:
    explicit Client(emberkv::net::Socket socket) : socket_(std::move(socket)) {}

    Reply call(const std::vector<std::string>& args) {
        emberkv::net::send_all(socket_.get(), emberkv::resp::encode_command(args));
        while (true) {
            if (auto parsed = emberkv::resp::parse_reply(buffer_)) {
                buffer_.erase(0, parsed->second);
                return std::move(parsed->first);
            }
            char chunk[16 * 1024];
            const auto r = emberkv::net::recv_some(socket_.get(), chunk, sizeof chunk);
            if (r.status != emberkv::net::IoResult::Status::ok) throw std::runtime_error("connection closed by server");
            buffer_.append(chunk, r.bytes);
        }
    }

private:
    emberkv::net::Socket socket_;
    std::string buffer_;
};

/// Splits a command line like a shell would: whitespace separates, "..." allows
/// spaces and escapes (\n \t \" \\), '...' is taken literally.
std::optional<std::vector<std::string>> tokenize(const std::string& line) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string token;
        if (line[i] == '"' || line[i] == '\'') {
            const char quote = line[i++];
            bool closed = false;
            while (i < line.size()) {
                char c = line[i++];
                if (c == quote) {
                    closed = true;
                    break;
                }
                if (quote == '"' && c == '\\' && i < line.size()) {
                    const char e = line[i++];
                    c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                }
                token.push_back(c);
            }
            if (!closed) return std::nullopt;
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') token.push_back(line[i++]);
        }
        out.push_back(std::move(token));
    }
    return out;
}

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char hex[5];
                    std::snprintf(hex, sizeof hex, "\\x%02x", c);
                    out += hex;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out + "\"";
}

/// Mirrors redis-cli's output format, including nested array numbering.
void print(const Reply& reply, std::size_t indent = 0) {
    using namespace emberkv::resp;
    std::visit(emberkv::overloaded{
                   [](const SimpleString& s) { std::cout << s.value << '\n'; },
                   [](const Error& e) { std::cout << "(error) " << e.message << '\n'; },
                   [](std::int64_t n) { std::cout << "(integer) " << n << '\n'; },
                   [](const std::string& s) {
                       if (s.find("\r\n") == std::string::npos) {
                           std::cout << quoted(s) << '\n';
                           return;
                       }
                       for (char c : s)  // multi-line text such as INFO: print it raw
                           if (c != '\r') std::cout << c;
                       if (s.back() != '\n') std::cout << '\n';
                   },
                   [](const Null&) { std::cout << "(nil)\n"; },
                   [&](const Array& items) {
                       if (items.empty()) {
                           std::cout << "(empty array)\n";
                           return;
                       }
                       const std::size_t width = std::to_string(items.size()).size();
                       for (std::size_t i = 0; i < items.size(); ++i) {
                           const std::string number = std::to_string(i + 1);
                           if (i > 0) std::cout << std::string(indent, ' ');
                           std::cout << std::string(width - number.size(), ' ') << number << ") ";
                           print(items[i], indent + width + 2);
                       }
                   },
               },
               reply.value);
}

void usage() {
    std::cout << "Usage: emberkv-cli [-h host] [-p port] [command [args...]]\n"
                 "With no command, starts an interactive prompt. Type HELP there for the command list.\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::uint16_t port = 6380;
    std::vector<std::string> command;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (command.empty() && arg == "--help") {
            usage();
            return 0;
        }
        if (command.empty() && arg == "-h" && i + 1 < argc) {
            host = argv[++i];
        } else if (command.empty() && arg == "-p" && i + 1 < argc) {
            port = static_cast<std::uint16_t>(std::stoul(argv[++i]));
        } else {
            command.push_back(arg);
        }
    }

    try {
        emberkv::net::NetworkInit network;
        Client client(emberkv::net::connect_tcp(host, port));

        if (!command.empty()) {
            const Reply reply = client.call(command);
            print(reply);
            return reply.is_error() ? 1 : 0;
        }

        const std::string prompt = host + ":" + std::to_string(port) + "> ";
        std::string line;
        while (std::cout << prompt << std::flush && std::getline(std::cin, line)) {
            auto args = tokenize(line);
            if (!args) {
                std::cout << "Invalid argument(s): unbalanced quotes\n";
                continue;
            }
            if (args->empty()) continue;
            std::string name = args->front();
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (name == "exit") break;
            print(client.call(*args));
            if (name == "quit") break;
        }
    } catch (const std::exception& e) {
        std::cerr << "emberkv-cli: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
