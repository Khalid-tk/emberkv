#include "emberkv/resp.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace emberkv;
using namespace emberkv::resp;
using Args = std::vector<std::string>;

// ------------------------------------------------------------------ encoding

TEST(RespEncode, EveryReplyType) {
    EXPECT_EQ(encode(Reply::ok()), "+OK\r\n");
    EXPECT_EQ(encode(Reply::error("ERR boom")), "-ERR boom\r\n");
    EXPECT_EQ(encode(Reply::integer(-42)), ":-42\r\n");
    EXPECT_EQ(encode(Reply::bulk("hello")), "$5\r\nhello\r\n");
    EXPECT_EQ(encode(Reply::bulk("")), "$0\r\n\r\n");
    EXPECT_EQ(encode(Reply::null()), "$-1\r\n");
    EXPECT_EQ(encode(Reply::array({})), "*0\r\n");
    EXPECT_EQ(encode(Reply::array({Reply::integer(1), Reply::array({Reply::bulk("x")})})), "*2\r\n:1\r\n*1\r\n$1\r\nx\r\n");
}

TEST(RespEncode, LineTypesCannotBreakTheStream) {
    EXPECT_EQ(encode(Reply::error("bad\r\nthing")), "-bad  thing\r\n");
}

TEST(RespEncode, BulkStringsAreBinarySafe) {
    const std::string binary("a\r\n\0b", 5);
    EXPECT_EQ(encode(Reply::bulk(binary)), "$5\r\n" + binary + "\r\n");
}

TEST(RespEncode, Command) {
    EXPECT_EQ(encode_command(Args{"SET", "k", "v"}), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
}

// ------------------------------------------------------------- request parse

TEST(RequestParser, ParsesOneRequest) {
    RequestParser p;
    p.feed("*2\r\n$3\r\nGET\r\n$3\r\nkey\r\n");
    EXPECT_EQ(p.next(), (Args{"GET", "key"}));
    EXPECT_EQ(p.next(), std::nullopt);
    EXPECT_EQ(p.buffered(), 0u);
}

TEST(RequestParser, HandlesInputSplitAtEveryByte) {
    const std::string wire = "*3\r\n$3\r\nSET\r\n$5\r\nhello\r\n$11\r\nhello world\r\n";
    RequestParser p;
    for (std::size_t i = 0; i + 1 < wire.size(); ++i) {
        p.feed(wire.substr(i, 1));
        ASSERT_EQ(p.next(), std::nullopt) << "complete too early at byte " << i;
    }
    p.feed(wire.substr(wire.size() - 1));
    EXPECT_EQ(p.next(), (Args{"SET", "hello", "hello world"}));
}

TEST(RequestParser, PipelinedRequestsComeOutInOrder) {
    RequestParser p;
    p.feed("*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\na\r\n*2\r\n$4\r\nECHO\r\n$1\r\nb");
    EXPECT_EQ(p.next(), (Args{"PING"}));
    EXPECT_EQ(p.next(), (Args{"GET", "a"}));
    EXPECT_EQ(p.next(), std::nullopt);  // third one is incomplete
    p.feed("\r\n");
    EXPECT_EQ(p.next(), (Args{"ECHO", "b"}));
}

TEST(RequestParser, BulkArgumentsAreBinarySafe) {
    RequestParser p;
    const char wire[] = "*2\r\n$4\r\nECHO\r\n$4\r\n\r\n\0x\r\n";
    p.feed(std::string(wire, sizeof wire - 1));  // sizeof, not strlen: the NUL is data
    EXPECT_EQ(p.next(), (Args{"ECHO", std::string("\r\n\0x", 4)}));
}

TEST(RequestParser, InlineCommands) {
    RequestParser p;
    p.feed("  SET   key\tvalue \r\n\r\nPING\r\n");
    EXPECT_EQ(p.next(), (Args{"SET", "key", "value"}));
    EXPECT_EQ(p.next(), (Args{"PING"}));  // the blank line is skipped
}

TEST(RequestParser, EmptyMultibulkIsANoOp) {
    RequestParser p;
    p.feed("*0\r\n*-1\r\n*1\r\n$4\r\nPING\r\n");
    EXPECT_EQ(p.next(), (Args{"PING"}));
}

TEST(RequestParser, RejectsMalformedInput) {
    auto fails = [](std::string input, RequestParser::Limits limits = {}) {
        RequestParser p(limits);
        p.feed(input);
        try {
            while (p.next()) {
            }
        } catch (const ProtocolError&) {
            return true;
        }
        return false;
    };
    EXPECT_TRUE(fails("*x\r\n"));
    EXPECT_TRUE(fails("*1\r\n+PING\r\n")) << "arguments must be bulk strings";
    EXPECT_TRUE(fails("*1\r\n$-5\r\n"));
    EXPECT_TRUE(fails("*1\r\n$3\r\nGETXX")) << "bulk data must end in CRLF";
    EXPECT_TRUE(fails("*1\r\n$100\r\n", {.max_bulk_length = 10}));
    EXPECT_TRUE(fails("*5\r\n", {.max_arguments = 4}));
    EXPECT_TRUE(fails(std::string(100, 'A'), {.max_inline_length = 64})) << "an endless line must not buffer forever";
}

// --------------------------------------------------------------- reply parse

TEST(ReplyParser, RoundTripsEveryType) {
    const Reply nested = Reply::array({Reply::ok(), Reply::error("ERR x"), Reply::integer(7), Reply::bulk("a\r\nb"),
                                       Reply::null(), Reply::array({Reply::bulk(""), Reply::array({})})});
    const std::string wire = encode(nested);
    auto parsed = parse_reply(wire);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->first, nested);
    EXPECT_EQ(parsed->second, wire.size());

    // Every strict prefix is incomplete, never an error and never a wrong value.
    for (std::size_t len = 0; len < wire.size(); ++len) EXPECT_EQ(parse_reply(wire.substr(0, len)), std::nullopt);
}

TEST(ReplyParser, ReportsBytesConsumedSoRepliesCanBeChained) {
    const std::string wire = "+OK\r\n:5\r\n";
    auto first = parse_reply(wire);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->second, 5u);
    auto second = parse_reply(std::string_view(wire).substr(first->second));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->first, Reply::integer(5));
}
