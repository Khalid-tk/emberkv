#include "emberkv/commands.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <format>
#include <limits>
#include <unordered_map>
#include <utility>

namespace emberkv {

using resp::Reply;
using Cache = Store::Cache;
using namespace std::chrono_literals;

namespace {

constexpr std::string_view kVersion = "1.0.0";
constexpr std::string_view kWrongType = "WRONGTYPE Operation against a key holding the wrong kind of value";
constexpr std::string_view kNotInteger = "ERR value is not an integer or out of range";
constexpr std::string_view kSyntax = "ERR syntax error";
constexpr std::string_view kOom = "OOM command not allowed when used memory > 'maxmemory'";
constexpr std::string_view kNoSnapshot = "ERR no snapshot file configured (start the server with --snapshot <file>)";

// Upper bound on TTLs. Keeps `now + ttl` far away from steady_clock overflow.
constexpr std::int64_t kMaxTtlMillis = 100LL * 365 * 24 * 3600 * 1000;

Reply err(std::string_view message) { return Reply::error(std::string(message)); }

std::string to_upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool iequals(std::string_view a, std::string_view b) noexcept {
    return std::ranges::equal(a, b, [](char x, char y) {
        return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
    });
}

std::optional<std::int64_t> to_int(std::string_view s) noexcept {
    std::int64_t value = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size()) return std::nullopt;
    return value;
}

std::vector<std::string_view> as_views(std::span<const std::string> args) {
    return {args.begin(), args.end()};
}

Reply wrong_args(std::string_view command) {
    return Reply::error("ERR wrong number of arguments for '" + to_lower(command) + "' command");
}

std::int64_t unix_seconds_now() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string human_bytes(std::size_t bytes) {
    constexpr std::array units{"B", "K", "M", "G", "T"};
    auto value = static_cast<double>(bytes);
    std::size_t unit = 0;
    while (value >= 1024 && unit + 1 < units.size()) {
        value /= 1024;
        ++unit;
    }
    return std::format("{:.2f}{}", value, units[unit]);
}

}  // namespace

// ------------------------------------------------------------- dispatch table

struct CommandProcessor::Spec {
    CommandInfo info;
    Handler handler;
};

std::span<const CommandProcessor::Spec> CommandProcessor::table() noexcept {
    using P = CommandProcessor;
    static const Spec kTable[] = {
        {{"PING", -1, "PING [message] - test the connection"}, &P::ping},
        {{"ECHO", 2, "ECHO message"}, &P::echo},
        {{"QUIT", 1, "QUIT - close the connection"}, &P::quit},
        {{"HELP", 1, "HELP - list commands"}, &P::help},
        {{"INFO", -1, "INFO [section] - server statistics"}, &P::info},
        {{"DBSIZE", 1, "DBSIZE - number of keys"}, &P::dbsize},
        {{"FLUSHALL", -1, "FLUSHALL - delete every key"}, &P::flushall},
        {{"FLUSHDB", -1, "FLUSHDB - alias of FLUSHALL"}, &P::flushall},
        {{"COMMAND", -1, "COMMAND [COUNT] - client compatibility"}, &P::command},
        {{"CONFIG", -2, "CONFIG GET pattern"}, &P::config},
        {{"CLIENT", -2, "CLIENT SETNAME|SETINFO|GETNAME - client compatibility"}, &P::client},
        {{"GET", 2, "GET key"}, &P::get},
        {{"SET", -3, "SET key value [EX seconds|PX ms|KEEPTTL] [NX|XX] [GET]"}, &P::set},
        {{"GETDEL", 2, "GETDEL key"}, &P::getdel},
        {{"APPEND", 3, "APPEND key value"}, &P::append},
        {{"STRLEN", 2, "STRLEN key"}, &P::strlen},
        {{"INCR", 2, "INCR key"}, &P::incr},
        {{"DECR", 2, "DECR key"}, &P::decr},
        {{"INCRBY", 3, "INCRBY key increment"}, &P::incrby},
        {{"DECRBY", 3, "DECRBY key decrement"}, &P::decrby},
        {{"MSET", -3, "MSET key value [key value ...]"}, &P::mset},
        {{"MGET", -2, "MGET key [key ...]"}, &P::mget},
        {{"DEL", -2, "DEL key [key ...]"}, &P::del},
        {{"EXISTS", -2, "EXISTS key [key ...]"}, &P::exists},
        {{"TYPE", 2, "TYPE key"}, &P::type},
        {{"KEYS", 2, "KEYS pattern - glob match, e.g. user:*"}, &P::keys},
        {{"EXPIRE", 3, "EXPIRE key seconds"}, &P::expire},
        {{"PEXPIRE", 3, "PEXPIRE key milliseconds"}, &P::pexpire},
        {{"TTL", 2, "TTL key - seconds left, -1 no expiry, -2 missing"}, &P::ttl},
        {{"PTTL", 2, "PTTL key - milliseconds left"}, &P::pttl},
        {{"PERSIST", 2, "PERSIST key - remove the expiry"}, &P::persist},
        {{"LPUSH", -3, "LPUSH key value [value ...]"}, &P::lpush},
        {{"RPUSH", -3, "RPUSH key value [value ...]"}, &P::rpush},
        {{"LPOP", -2, "LPOP key [count]"}, &P::lpop},
        {{"RPOP", -2, "RPOP key [count]"}, &P::rpop},
        {{"LLEN", 2, "LLEN key"}, &P::llen},
        {{"LRANGE", 4, "LRANGE key start stop"}, &P::lrange},
        {{"SAVE", 1, "SAVE - write a snapshot now"}, &P::save},
        {{"BGSAVE", -1, "BGSAVE - write a snapshot in the background"}, &P::bgsave},
        {{"LASTSAVE", 1, "LASTSAVE - unix time of the last successful save"}, &P::lastsave},
    };
    return kTable;
}

std::span<const CommandProcessor::CommandInfo> CommandProcessor::commands() noexcept {
    // Function-local statics are initialised exactly once, thread-safely.
    static const std::vector<CommandInfo> infos = [] {
        std::vector<CommandInfo> v;
        for (const Spec& s : table()) v.push_back(s.info);
        return v;
    }();
    return infos;
}

const CommandProcessor::Spec* CommandProcessor::lookup(std::string_view name) {
    static const auto index = [] {
        std::unordered_map<std::string_view, const Spec*> m;
        for (const Spec& s : table()) m.emplace(s.info.name, &s);
        return m;
    }();
    if (name.size() > 16) return nullptr;
    auto it = index.find(to_upper(name));
    return it == index.end() ? nullptr : it->second;
}

CommandProcessor::CommandProcessor(Store& store, Options options) : store_(store), options_(std::move(options)) {}

CommandProcessor::~CommandProcessor() { wait_for_background_save(); }

Reply CommandProcessor::execute(std::vector<std::string> argv) {
    if (argv.empty()) return err("ERR empty command");
    if (options_.counters) options_.counters->total_commands.fetch_add(1, std::memory_order_relaxed);

    const Spec* spec = lookup(argv[0]);
    if (!spec) return Reply::error("ERR unknown command '" + argv[0].substr(0, 64) + "'");

    const auto argc = static_cast<std::int64_t>(argv.size());
    const int arity = spec->info.arity;
    if ((arity > 0 && argc != arity) || (arity < 0 && argc < -arity)) return wrong_args(spec->info.name);

    try {
        return (this->*(spec->handler))(Args{argv});
    } catch (const std::bad_alloc&) {
        return err(kOom);
    } catch (const std::exception& e) {
        return Reply::error(std::string("ERR ") + e.what());
    }
}

// ------------------------------------------------------- connection / server

Reply CommandProcessor::ping(Args args) {
    if (args.size() > 2) return wrong_args("ping");
    return args.size() == 2 ? Reply::bulk(std::move(args[1])) : Reply::simple("PONG");
}

Reply CommandProcessor::echo(Args args) { return Reply::bulk(std::move(args[1])); }

Reply CommandProcessor::quit(Args) { return Reply::ok(); }

Reply CommandProcessor::help(Args) {
    resp::Array lines;
    for (const auto& c : commands()) lines.push_back(Reply::bulk(std::string(c.summary)));
    return Reply::array(std::move(lines));
}

Reply CommandProcessor::dbsize(Args) { return Reply::integer(static_cast<std::int64_t>(store_.stats().keys)); }

Reply CommandProcessor::flushall(Args) {
    store_.clear();
    return Reply::ok();
}

Reply CommandProcessor::command(Args args) {
    // redis-cli asks for COMMAND DOCS on startup to drive its hints; an empty
    // reply means "no hints" rather than an error message in the terminal.
    if (args.size() >= 2 && iequals(args[1], "COUNT"))
        return Reply::integer(static_cast<std::int64_t>(table().size()));
    return Reply::array({});
}

Reply CommandProcessor::config(Args args) {
    if (!iequals(args[1], "GET")) return err("ERR only CONFIG GET is supported");
    if (args.size() != 3) return wrong_args("config|get");
    const std::pair<std::string_view, std::string> params[] = {
        {"maxmemory", std::to_string(store_.stats().max_memory)},
        {"maxmemory-policy", "allkeys-lru"},
        {"save", ""},  // redis-benchmark reads these two on startup
        {"appendonly", "no"},
        {"databases", "1"},
    };
    resp::Array out;
    for (const auto& [name, value] : params) {
        if (glob_match(to_lower(args[2]), name)) {
            out.push_back(Reply::bulk(std::string(name)));
            out.push_back(Reply::bulk(value));
        }
    }
    return Reply::array(std::move(out));
}

Reply CommandProcessor::client(Args args) {
    if (iequals(args[1], "SETNAME") || iequals(args[1], "SETINFO")) return Reply::ok();
    if (iequals(args[1], "GETNAME")) return Reply::null();
    return Reply::error("ERR unknown subcommand '" + args[1].substr(0, 32) + "'");
}

Reply CommandProcessor::info(Args args) {
    if (args.size() > 2) return wrong_args("info");
    const StoreStats s = store_.stats();
    const auto started = options_.counters ? options_.counters->started_at : started_at_;
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    auto load = [](const std::atomic<std::uint64_t>* a) { return a ? a->load(std::memory_order_relaxed) : 0; };
    ServerCounters* c = options_.counters;

    std::vector<std::pair<std::string_view, std::string>> sections;
    sections.emplace_back("Server", std::format("emberkv_version:{}\r\nredis_version:7.0.0\r\nuptime_in_seconds:{}\r\n"
                                                "io_threads:{}\r\nshards:{}\r\n",
                                                kVersion, uptime.count(), options_.io_threads, store_.shard_count()));
    sections.emplace_back("Clients", std::format("connected_clients:{}\r\n", load(c ? &c->connected_clients : nullptr)));
    sections.emplace_back("Memory", std::format("used_memory:{}\r\nused_memory_human:{}\r\nmaxmemory:{}\r\n"
                                                "maxmemory_human:{}\r\nmaxmemory_policy:allkeys-lru\r\n",
                                                s.used_memory, human_bytes(s.used_memory), s.max_memory,
                                                human_bytes(s.max_memory)));
    sections.emplace_back("Persistence",
                          std::format("rdb_last_save_time:{}\r\nrdb_last_bgsave_status:{}\r\nrdb_bgsave_in_progress:{}\r\n",
                                      last_save_unix_.load(), last_save_ok_.load() ? "ok" : "err",
                                      bgsave_running_.load() ? 1 : 0));
    sections.emplace_back("Stats",
                          std::format("total_connections_received:{}\r\ntotal_commands_processed:{}\r\n"
                                      "rejected_connections:{}\r\nkeyspace_hits:{}\r\nkeyspace_misses:{}\r\n"
                                      "evicted_keys:{}\r\nexpired_keys:{}\r\n",
                                      load(c ? &c->total_connections : nullptr),
                                      load(c ? &c->total_commands : nullptr),
                                      load(c ? &c->rejected_connections : nullptr), s.hits, s.misses, s.evictions,
                                      s.expirations));
    sections.emplace_back("Keyspace", std::format("db0:keys={},expires={}\r\n", s.keys, s.keys_with_expiry));

    std::string out;
    for (const auto& [name, body] : sections) {
        if (args.size() == 2 && !iequals(args[1], name) && !iequals(args[1], "all")) continue;
        if (!out.empty()) out += "\r\n";
        out += std::format("# {}\r\n{}", name, body);
    }
    return Reply::bulk(std::move(out));
}

// ---------------------------------------------------------- strings / counters

Reply CommandProcessor::get(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        store_.record_lookup(e != nullptr);
        if (!e) return Reply::null();
        if (const auto* s = std::get_if<StringValue>(&e->value())) return Reply::bulk(*s);
        return err(kWrongType);
    });
}

Reply CommandProcessor::set(Args args) {
    std::optional<std::chrono::milliseconds> ttl;
    bool nx = false, xx = false, keepttl = false, get_old = false;

    for (std::size_t i = 3; i < args.size(); ++i) {
        const std::string opt = to_upper(args[i]);
        if (opt == "NX" && !xx) {
            nx = true;
        } else if (opt == "XX" && !nx) {
            xx = true;
        } else if (opt == "KEEPTTL" && !ttl) {
            keepttl = true;
        } else if (opt == "GET") {
            get_old = true;
        } else if ((opt == "EX" || opt == "PX") && !ttl && !keepttl && i + 1 < args.size()) {
            const auto unit = opt == "EX" ? std::chrono::milliseconds{1000} : std::chrono::milliseconds{1};
            const auto n = to_int(args[++i]);
            if (!n) return err(kNotInteger);
            if (*n <= 0 || *n > kMaxTtlMillis / unit.count()) return err("ERR invalid expire time in 'set' command");
            ttl = unit * *n;
        } else {
            return err(kSyntax);
        }
    }

    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        Reply old = Reply::null();
        if (get_old && e) {
            const auto* s = std::get_if<StringValue>(&e->value());
            if (!s) return err(kWrongType);
            old = Reply::bulk(*s);
        }
        if ((nx && e) || (xx && !e)) return get_old ? old : Reply::null();

        std::optional<TimePoint> deadline;
        if (ttl) deadline = now + *ttl;
        else if (keepttl && e) deadline = e->expires_at();

        if (c.put(std::move(args[1]), std::move(args[2]), deadline) == Cache::PutResult::too_large) return err(kOom);
        return get_old ? old : Reply::ok();
    });
}

Reply CommandProcessor::getdel(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        store_.record_lookup(e != nullptr);
        if (!e) return Reply::null();
        auto* s = std::get_if<StringValue>(&e->value());
        if (!s) return err(kWrongType);
        Reply r = Reply::bulk(std::move(*s));
        c.erase(key);
        return r;
    });
}

Reply CommandProcessor::append(Args args) {
    const std::string& key = args[1];
    std::string& value = args[2];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        if (!e) {
            const auto len = static_cast<std::int64_t>(value.size());
            if (c.put(key, std::move(value)) == Cache::PutResult::too_large) return err(kOom);
            return Reply::integer(len);
        }
        auto* s = std::get_if<StringValue>(&e->value());
        if (!s) return err(kWrongType);
        if (e->weight() + value.size() > c.capacity()) return err(kOom);  // check before mutating
        s->append(value);
        const auto len = static_cast<std::int64_t>(s->size());
        if (!c.reweigh(*e)) return err(kOom);
        return Reply::integer(len);
    });
}

Reply CommandProcessor::strlen(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        const Cache::Entry* e = c.peek(key, now);
        if (!e) return Reply::integer(0);
        const auto* s = std::get_if<StringValue>(&e->value());
        return s ? Reply::integer(static_cast<std::int64_t>(s->size())) : err(kWrongType);
    });
}

Reply CommandProcessor::incr_by(std::string_view key, std::int64_t delta) {
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        if (!e) {
            if (c.put(std::string(key), std::to_string(delta)) == Cache::PutResult::too_large) return err(kOom);
            return Reply::integer(delta);
        }
        auto* s = std::get_if<StringValue>(&e->value());
        if (!s) return err(kWrongType);
        const auto current = to_int(*s);
        if (!current) return err(kNotInteger);

        constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
        constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
        // Check *before* adding: signed overflow is undefined behaviour.
        if ((delta > 0 && *current > kMax - delta) || (delta < 0 && *current < kMin - delta))
            return err("ERR increment or decrement would overflow");

        const std::int64_t result = *current + delta;
        *s = std::to_string(result);
        c.reweigh(*e);  // the TTL is preserved, as in Redis
        return Reply::integer(result);
    });
}

Reply CommandProcessor::incr(Args args) { return incr_by(args[1], 1); }
Reply CommandProcessor::decr(Args args) { return incr_by(args[1], -1); }

Reply CommandProcessor::incrby(Args args) {
    const auto delta = to_int(args[2]);
    return delta ? incr_by(args[1], *delta) : err(kNotInteger);
}

Reply CommandProcessor::decrby(Args args) {
    const auto delta = to_int(args[2]);
    if (!delta) return err(kNotInteger);
    if (*delta == std::numeric_limits<std::int64_t>::min()) return err("ERR decrement would overflow");
    return incr_by(args[1], -*delta);
}

Reply CommandProcessor::mset(Args args) {
    if (args.size() % 2 == 0) return wrong_args("mset");
    std::vector<std::string_view> keys;
    for (std::size_t i = 1; i < args.size(); i += 2) keys.push_back(args[i]);

    return store_.with_keys(keys, [&](std::span<Cache* const> caches, TimePoint) -> Reply {
        // Validate everything first so MSET is all-or-nothing.
        for (std::size_t k = 0; k < caches.size(); ++k) {
            const std::string& key = args[1 + 2 * k];
            const std::string& value = args[2 + 2 * k];
            const std::size_t weight = EntryWeigher::kEntryOverhead + key.size() + sizeof(std::string) + value.size();
            if (weight > caches[k]->capacity()) return err(kOom);
        }
        // Keys and values are moved into the store: MSET copies nothing.
        for (std::size_t k = 0; k < caches.size(); ++k)
            caches[k]->put(std::move(args[1 + 2 * k]), std::move(args[2 + 2 * k]));
        return Reply::ok();
    });
}

Reply CommandProcessor::mget(Args args) {
    const auto keys = as_views(args.subspan(1));
    return store_.with_keys(keys, [&](std::span<Cache* const> caches, TimePoint now) -> Reply {
        resp::Array out;
        out.reserve(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            Cache::Entry* e = caches[i]->find(keys[i], now);
            store_.record_lookup(e != nullptr);
            const auto* s = e ? std::get_if<StringValue>(&e->value()) : nullptr;
            out.push_back(s ? Reply::bulk(*s) : Reply::null());  // wrong type reads as nil, like Redis
        }
        return Reply::array(std::move(out));
    });
}

// ------------------------------------------------------------------ keyspace

Reply CommandProcessor::del(Args args) {
    const auto keys = as_views(args.subspan(1));
    return store_.with_keys(keys, [&](std::span<Cache* const> caches, TimePoint now) -> Reply {
        std::int64_t removed = 0;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (caches[i]->find(keys[i], now)) {  // find() first so an expired key doesn't count
                caches[i]->erase(keys[i]);
                ++removed;
            }
        }
        return Reply::integer(removed);
    });
}

Reply CommandProcessor::exists(Args args) {
    const auto keys = as_views(args.subspan(1));
    return store_.with_keys(keys, [&](std::span<Cache* const> caches, TimePoint now) -> Reply {
        std::int64_t found = 0;
        for (std::size_t i = 0; i < keys.size(); ++i) found += caches[i]->peek(keys[i], now) ? 1 : 0;
        return Reply::integer(found);
    });
}

Reply CommandProcessor::type(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) {
        const Cache::Entry* e = c.peek(key, now);
        return Reply::simple(e ? std::string(type_name(e->value())) : "none");
    });
}

Reply CommandProcessor::keys(Args args) {
    const std::string& pattern = args[1];
    std::vector<std::string> found;
    store_.for_each_shard([&](Cache& c, TimePoint now) {
        c.for_each(now, [&](const Cache::Entry& e) {
            if (glob_match(pattern, e.key())) found.push_back(e.key());
        });
    });
    std::ranges::sort(found);
    resp::Array out;
    out.reserve(found.size());
    for (auto& k : found) out.push_back(Reply::bulk(std::move(k)));
    return Reply::array(std::move(out));
}

Reply CommandProcessor::expire_in(std::string_view key, std::int64_t amount, std::chrono::milliseconds unit) {
    if (amount > kMaxTtlMillis / unit.count()) return err("ERR invalid expire time");
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        if (!e) return Reply::integer(0);
        if (amount <= 0) {  // a deadline in the past deletes the key, as in Redis
            c.erase(key);
            return Reply::integer(1);
        }
        c.set_expiry(*e, now + unit * amount);
        return Reply::integer(1);
    });
}

Reply CommandProcessor::expire(Args args) {
    const auto n = to_int(args[2]);
    return n ? expire_in(args[1], *n, 1000ms) : err(kNotInteger);
}

Reply CommandProcessor::pexpire(Args args) {
    const auto n = to_int(args[2]);
    return n ? expire_in(args[1], *n, 1ms) : err(kNotInteger);
}

Reply CommandProcessor::ttl_in(std::string_view key, bool millis) {
    return store_.with_key(key, [&](Cache& c, TimePoint now) {
        const Cache::Entry* e = c.peek(key, now);
        if (!e) return Reply::integer(-2);
        const auto at = e->expires_at();
        if (!at) return Reply::integer(-1);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(*at - now).count();
        return Reply::integer(millis ? ms : (ms + 500) / 1000);
    });
}

Reply CommandProcessor::ttl(Args args) { return ttl_in(args[1], false); }
Reply CommandProcessor::pttl(Args args) { return ttl_in(args[1], true); }

Reply CommandProcessor::persist(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) {
        Cache::Entry* e = c.find(key, now);
        if (!e || !e->expires_at()) return Reply::integer(0);
        c.set_expiry(*e, std::nullopt);
        return Reply::integer(1);
    });
}

// --------------------------------------------------------------------- lists

Reply CommandProcessor::push(Args args, bool front) {
    const std::string& key = args[1];
    const auto values = args.subspan(2);
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        auto push_all = [&](ListValue& list) {
            for (std::string& v : values) front ? list.push_front(std::move(v)) : list.push_back(std::move(v));
        };
        if (!e) {
            ListValue list;
            push_all(list);
            const auto n = static_cast<std::int64_t>(list.size());
            if (c.put(key, std::move(list)) == Cache::PutResult::too_large) return err(kOom);
            return Reply::integer(n);
        }
        auto* list = std::get_if<ListValue>(&e->value());
        if (!list) return err(kWrongType);

        std::size_t added = 0;
        for (const auto& v : values) added += sizeof(std::string) + v.size();
        if (e->weight() + added > c.capacity()) return err(kOom);

        push_all(*list);
        const auto n = static_cast<std::int64_t>(list->size());
        c.reweigh(*e);
        return Reply::integer(n);
    });
}

Reply CommandProcessor::pop(Args args, bool front) {
    if (args.size() > 3) return wrong_args(front ? "lpop" : "rpop");
    std::optional<std::int64_t> count;
    if (args.size() == 3) {
        count = to_int(args[2]);
        if (!count || *count < 0) return err("ERR value is out of range, must be positive");
    }

    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        Cache::Entry* e = c.find(key, now);
        if (!e) return Reply::null();
        auto* list = std::get_if<ListValue>(&e->value());
        if (!list) return err(kWrongType);

        auto take = [&] { return front ? list->pop_front() : list->pop_back(); };
        Reply result;
        if (!count) {
            result = Reply::bulk(take());
        } else {
            resp::Array items;
            const auto n = std::min(static_cast<std::size_t>(*count), list->size());
            items.reserve(n);
            for (std::size_t i = 0; i < n; ++i) items.push_back(Reply::bulk(take()));
            result = Reply::array(std::move(items));
        }
        if (list->empty()) c.erase(key);  // empty lists don't exist, as in Redis
        else c.reweigh(*e);
        return result;
    });
}

Reply CommandProcessor::lpush(Args args) { return push(args, true); }
Reply CommandProcessor::rpush(Args args) { return push(args, false); }
Reply CommandProcessor::lpop(Args args) { return pop(args, true); }
Reply CommandProcessor::rpop(Args args) { return pop(args, false); }

Reply CommandProcessor::llen(Args args) {
    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) {
        const Cache::Entry* e = c.peek(key, now);
        if (!e) return Reply::integer(0);
        const auto* list = std::get_if<ListValue>(&e->value());
        return list ? Reply::integer(static_cast<std::int64_t>(list->size())) : err(kWrongType);
    });
}

Reply CommandProcessor::lrange(Args args) {
    const auto start_arg = to_int(args[2]);
    const auto stop_arg = to_int(args[3]);
    if (!start_arg || !stop_arg) return err(kNotInteger);

    const std::string& key = args[1];
    return store_.with_key(key, [&](Cache& c, TimePoint now) -> Reply {
        const Cache::Entry* e = c.peek(key, now);
        if (!e) return Reply::array({});
        const auto* list = std::get_if<ListValue>(&e->value());
        if (!list) return err(kWrongType);

        // Negative indexes count from the end: -1 is the last element.
        const auto n = static_cast<std::int64_t>(list->size());
        std::int64_t start = *start_arg < 0 ? *start_arg + n : *start_arg;
        std::int64_t stop = *stop_arg < 0 ? *stop_arg + n : *stop_arg;
        start = std::max<std::int64_t>(start, 0);
        stop = std::min<std::int64_t>(stop, n - 1);
        if (start > stop) return Reply::array({});

        resp::Array out;
        out.reserve(static_cast<std::size_t>(stop - start + 1));
        for (auto i = start; i <= stop; ++i) out.push_back(Reply::bulk((*list)[static_cast<std::size_t>(i)]));
        return Reply::array(std::move(out));
    });
}

// --------------------------------------------------------------- persistence

void CommandProcessor::record_save(const SnapshotInfo& info) {
    last_save_unix_.store(unix_seconds_now());
    last_save_keys_.store(info.keys);
    last_save_ok_.store(true);
}

Reply CommandProcessor::save(Args) {
    if (!options_.snapshot_path) return err(kNoSnapshot);
    std::scoped_lock lock(save_mutex_);
    if (bgsave_.valid()) bgsave_.wait();  // never two writers on the same .tmp file
    try {
        record_save(save_snapshot(store_, *options_.snapshot_path));
        return Reply::ok();
    } catch (const std::exception& e) {
        last_save_ok_.store(false);
        return Reply::error(std::string("ERR ") + e.what());
    }
}

Reply CommandProcessor::bgsave(Args) {
    if (!options_.snapshot_path) return err(kNoSnapshot);
    std::scoped_lock lock(save_mutex_);
    if (bgsave_.valid() && bgsave_.wait_for(0s) != std::future_status::ready)
        return err("ERR Background save already in progress");
    bgsave_running_.store(true);
    bgsave_ = saver_.submit([this] {
        try {
            record_save(save_snapshot(store_, *options_.snapshot_path));
        } catch (const std::exception&) {
            last_save_ok_.store(false);
        }
        bgsave_running_.store(false);
    });
    return Reply::simple("Background saving started");
}

Reply CommandProcessor::lastsave(Args) { return Reply::integer(last_save_unix_.load()); }

void CommandProcessor::wait_for_background_save() {
    std::scoped_lock lock(save_mutex_);
    if (bgsave_.valid()) bgsave_.wait();
}

// --------------------------------------------------------------------- glob

bool glob_match(std::string_view p, std::string_view s) noexcept {
    // Iterative matcher with single-point backtracking for '*': linear-ish in
    // practice and, unlike the naive recursive version, no exponential blowup.
    constexpr auto npos = std::string_view::npos;
    std::size_t pi = 0, si = 0, star_p = npos, star_s = 0;

    while (si < s.size()) {
        if (pi < p.size()) {
            const char c = p[pi];
            if (c == '*') {
                star_p = pi++;
                star_s = si;
                continue;
            }
            if (c == '?') {
                ++pi;
                ++si;
                continue;
            }
            if (c == '[') {
                std::size_t j = pi + 1;
                const bool negate = j < p.size() && p[j] == '^';
                if (negate) ++j;
                bool matched = false;
                const auto ch = static_cast<unsigned char>(s[si]);
                while (j < p.size() && p[j] != ']') {
                    if (p[j] == '\\' && j + 1 < p.size()) {
                        matched |= static_cast<unsigned char>(p[j + 1]) == ch;
                        j += 2;
                    } else if (j + 2 < p.size() && p[j + 1] == '-' && p[j + 2] != ']') {
                        auto lo = static_cast<unsigned char>(p[j]);
                        auto hi = static_cast<unsigned char>(p[j + 2]);
                        if (lo > hi) std::swap(lo, hi);
                        matched |= ch >= lo && ch <= hi;
                        j += 3;
                    } else {
                        matched |= static_cast<unsigned char>(p[j]) == ch;
                        ++j;
                    }
                }
                if (j < p.size() && matched != negate) {  // class closed and satisfied
                    pi = j + 1;
                    ++si;
                    continue;
                }
                if (j >= p.size() && s[si] == '[') {  // unterminated '[' is a literal
                    ++pi;
                    ++si;
                    continue;
                }
            } else {
                std::size_t lit = pi;
                if (c == '\\' && pi + 1 < p.size()) ++lit;
                if (p[lit] == s[si]) {
                    pi = lit + 1;
                    ++si;
                    continue;
                }
            }
        }
        if (star_p == npos) return false;
        pi = star_p + 1;  // let the last '*' swallow one more character
        si = ++star_s;
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

}  // namespace emberkv
