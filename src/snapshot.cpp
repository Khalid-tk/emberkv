#include "emberkv/snapshot.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace emberkv {

namespace {

constexpr std::array<char, 8> kMagic{'E', 'M', 'B', 'E', 'R', 'K', 'V', '\0'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint8_t kTypeString = 0;
constexpr std::uint8_t kTypeList = 1;
constexpr std::uint8_t kEndMarker = 0xFF;

std::uint64_t fnv1a(std::string_view bytes) noexcept {
    std::uint64_t h = 14695981039346656037ull;
    for (char c : bytes) {
        h ^= static_cast<unsigned char>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::int64_t unix_millis_now() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

/// Serialises integers byte by byte, so the output is little-endian no matter
/// what the host's byte order is (no reinterpret_cast, no UB).
class Writer {
public:
    explicit Writer(std::string& out) : out_(out) {}

    void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v) { put(v, 4); }
    void u64(std::uint64_t v) { put(v, 8); }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void str(std::string_view s) {
        if (s.size() > std::numeric_limits<std::uint32_t>::max()) throw SnapshotError("value too large to snapshot");
        u32(static_cast<std::uint32_t>(s.size()));
        out_.append(s);
    }

private:
    void put(std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) out_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
    std::string& out_;
};

class Reader {
public:
    explicit Reader(std::string_view in) : in_(in) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(get(4)); }
    std::uint64_t u64() { return get(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    std::string str() { return std::string(take(u32())); }
    std::string_view take(std::size_t n) {
        if (in_.size() - pos_ < n) throw SnapshotError("snapshot is truncated");
        auto s = in_.substr(pos_, n);
        pos_ += n;
        return s;
    }
    [[nodiscard]] bool at_end() const noexcept { return pos_ == in_.size(); }

private:
    std::uint64_t get(int bytes) {
        auto raw = take(static_cast<std::size_t>(bytes));
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i)
            v |= static_cast<std::uint64_t>(static_cast<unsigned char>(raw[static_cast<std::size_t>(i)])) << (8 * i);
        return v;
    }
    std::string_view in_;
    std::size_t pos_ = 0;
};

struct Record {
    std::string key;
    Value value;
    std::optional<std::int64_t> expires_unix_ms;
};

}  // namespace

SnapshotInfo save_snapshot(Store& store, const std::filesystem::path& path) {
    std::string data;
    Writer w(data);
    data.append(kMagic.data(), kMagic.size());
    w.u32(kVersion);

    const std::int64_t wall_now = unix_millis_now();
    SnapshotInfo info;

    // Serialise one shard at a time: each shard's lock is held only while its
    // own entries are encoded, so the server keeps serving the others.
    store.for_each_shard([&](Store::Cache& cache, TimePoint now) {
        cache.for_each(now, [&](const Store::Cache::Entry& e) {
            std::int64_t expiry = -1;
            if (auto at = e.expires_at()) {
                using namespace std::chrono;
                expiry = wall_now + duration_cast<milliseconds>(*at - now).count();
            }
            std::visit(overloaded{[&](const StringValue& s) {
                                      w.u8(kTypeString);
                                      w.i64(expiry);
                                      w.str(e.key());
                                      w.str(s);
                                  },
                                  [&](const ListValue& list) {
                                      w.u8(kTypeList);
                                      w.i64(expiry);
                                      w.str(e.key());
                                      w.u32(static_cast<std::uint32_t>(list.size()));
                                      for (const auto& item : list) w.str(item);
                                  }},
                       e.value());
            ++info.keys;
        });
    });

    w.u8(kEndMarker);
    w.u64(fnv1a(data));
    info.bytes = data.size();

    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) throw SnapshotError("cannot open " + tmp.string() + " for writing");
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out) throw SnapshotError("failed writing " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);  // atomic replace on POSIX and Windows
    if (ec) throw SnapshotError("cannot rename snapshot into place: " + ec.message());
    return info;
}

SnapshotInfo load_snapshot(Store& store, const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw SnapshotError("cannot open " + path.string());
    const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};

    constexpr std::size_t kMinSize = kMagic.size() + 4 + 1 + 8;
    if (data.size() < kMinSize) throw SnapshotError("snapshot is truncated");
    if (std::string_view(data).substr(0, kMagic.size()) != std::string_view(kMagic.data(), kMagic.size()))
        throw SnapshotError("not an EmberKV snapshot");

    // Verify integrity before trusting a single length field.
    const std::string_view body = std::string_view(data).substr(0, data.size() - 8);
    Reader trailer(std::string_view(data).substr(data.size() - 8));
    if (trailer.u64() != fnv1a(body)) throw SnapshotError("snapshot checksum mismatch (file is corrupt)");

    Reader r(body);
    r.take(kMagic.size());
    if (const auto version = r.u32(); version != kVersion)
        throw SnapshotError("unsupported snapshot version " + std::to_string(version));

    std::vector<Record> records;
    while (true) {
        const std::uint8_t type = r.u8();
        if (type == kEndMarker) break;
        Record rec;
        const std::int64_t expiry = r.i64();
        if (expiry >= 0) rec.expires_unix_ms = expiry;
        rec.key = r.str();
        if (type == kTypeString) {
            rec.value = r.str();
        } else if (type == kTypeList) {
            ListValue list;
            for (std::uint32_t n = r.u32(); n > 0; --n) list.push_back(r.str());
            rec.value = std::move(list);
        } else {
            throw SnapshotError("unknown record type " + std::to_string(type));
        }
        records.push_back(std::move(rec));
    }
    if (!r.at_end()) throw SnapshotError("trailing bytes after end marker");

    // Parsing succeeded: only now modify the store.
    const std::int64_t wall_now = unix_millis_now();
    SnapshotInfo info{0, data.size()};
    for (Record& rec : records) {
        std::optional<std::chrono::milliseconds> ttl;
        if (rec.expires_unix_ms) {
            if (*rec.expires_unix_ms <= wall_now) continue;  // expired while we were down
            ttl = std::chrono::milliseconds{*rec.expires_unix_ms - wall_now};
        }
        store.with_key(rec.key, [&](Store::Cache& cache, TimePoint now) {
            std::optional<TimePoint> deadline;
            if (ttl) deadline = now + *ttl;
            if (cache.put(rec.key, std::move(rec.value), deadline) != Store::Cache::PutResult::too_large) ++info.keys;
        });
    }
    return info;
}

}  // namespace emberkv
