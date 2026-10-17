#include "emberkv/commands.hpp"
#include "emberkv/snapshot.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace emberkv;
using namespace emberkv::testing;

class Snapshot : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path = std::filesystem::temp_directory_path() / (std::string("emberkv-") + info->name() + ".ekv");
        std::filesystem::remove(path);
    }
    void TearDown() override {
        std::filesystem::remove(path);
        std::filesystem::remove(path.string() + ".tmp");
    }

    static std::string read_file(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    static void write_file(const std::filesystem::path& p, const std::string& data) {
        std::ofstream(p, std::ios::binary | std::ios::trunc) << data;
    }

    std::filesystem::path path;
};

TEST_F(Snapshot, RoundTripsStringsListsAndTtls) {
    {
        Store store;
        CommandProcessor p(store);
        (void)p.execute({"SET", "greeting", "hello"});
        (void)p.execute({"SET", "binary", std::string("a\0b\r\n", 5)});
        (void)p.execute({"RPUSH", "queue", "one", "two", "three"});
        (void)p.execute({"SET", "session", "abc", "EX", "3600"});
        const SnapshotInfo info = save_snapshot(store, path);
        EXPECT_EQ(info.keys, 4u);
    }
    EXPECT_FALSE(std::filesystem::exists(path.string() + ".tmp")) << "temp file renamed into place";

    Store restored;
    CommandProcessor p(restored);
    EXPECT_EQ(load_snapshot(restored, path).keys, 4u);
    EXPECT_EQ(p.execute({"GET", "greeting"}), bulk("hello"));
    EXPECT_EQ(p.execute({"GET", "binary"}), bulk(std::string("a\0b\r\n", 5)));
    EXPECT_EQ(p.execute({"LRANGE", "queue", "0", "-1"}), array({bulk("one"), bulk("two"), bulk("three")}));
    const auto ttl = std::get<std::int64_t>(p.execute({"TTL", "session"}).value);
    EXPECT_GE(ttl, 3598);
    EXPECT_LE(ttl, 3600);
    EXPECT_EQ(p.execute({"TTL", "greeting"}), integer(-1));
}

TEST_F(Snapshot, KeysThatExpiredWhileDownAreSkipped) {
    {
        Store store;
        CommandProcessor p(store);
        (void)p.execute({"SET", "short", "v", "PX", "20"});
        (void)p.execute({"SET", "long", "v"});
        save_snapshot(store, path);
    }
    std::this_thread::sleep_for(50ms);  // wall-clock time must pass: TTLs are stored as wall-clock deadlines
    Store restored;
    EXPECT_EQ(load_snapshot(restored, path).keys, 1u);
}

TEST_F(Snapshot, EmptyStore) {
    Store store;
    save_snapshot(store, path);
    Store restored;
    EXPECT_EQ(load_snapshot(restored, path).keys, 0u);
}

TEST_F(Snapshot, CorruptionIsDetectedAndLeavesTheStoreUntouched) {
    {
        Store store;
        CommandProcessor p(store);
        (void)p.execute({"SET", "k", "v"});
        save_snapshot(store, path);
    }
    std::string data = read_file(path);
    data[data.size() / 2] ^= 0x01;  // flip one bit
    write_file(path, data);

    Store restored;
    CommandProcessor p(restored);
    (void)p.execute({"SET", "existing", "kept"});
    EXPECT_THROW(load_snapshot(restored, path), SnapshotError);
    EXPECT_EQ(restored.stats().keys, 1u);
}

TEST_F(Snapshot, RejectsTruncatedAndForeignFiles) {
    Store store;
    write_file(path, "EMBERKV");
    EXPECT_THROW(load_snapshot(store, path), SnapshotError);
    write_file(path, std::string(64, 'x'));
    EXPECT_THROW(load_snapshot(store, path), SnapshotError);
    EXPECT_THROW(load_snapshot(store, path.string() + ".does-not-exist"), SnapshotError);
}
