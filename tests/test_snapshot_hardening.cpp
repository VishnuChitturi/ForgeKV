// =============================================================================
// ForgeKV — Stage 13: Snapshot Hardening Tests
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"
#include "forgekv/snapshot.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// =============================================================================
// Minimal test harness
// =============================================================================

struct TestCase { std::string name; std::function<void()> fn; };
static std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> r; return r;
}
struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};
struct AssertionFailure { std::string message; };

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) throw AssertionFailure{"ASSERT_TRUE failed: " #cond " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_FALSE(cond) \
    do { if ((cond)) throw AssertionFailure{"ASSERT_FALSE failed: " #cond " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_EQ(a,b) \
    do { if ((a)!=(b)) throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_HAS_VALUE(opt) \
    do { if (!(opt).has_value()) throw AssertionFailure{"ASSERT_HAS_VALUE failed: " #opt " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_NO_VALUE(opt) \
    do { if ((opt).has_value()) throw AssertionFailure{"ASSERT_NO_VALUE failed: " #opt " (line " + std::to_string(__LINE__) + ")"}; } while(false)

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar registrar_##name{#name, test_##name}; \
    static void test_##name()

// =============================================================================
// Helpers
// =============================================================================

struct SnapFiles {
    std::string wal;
    std::string snap;
    explicit SnapFiles(const std::string& name)
        : wal("test_sh_" + name + ".wal")
        , snap("test_sh_" + name + ".wal.snapshot") {}
    ~SnapFiles() {
        std::error_code ec;
        std::filesystem::remove(wal, ec);
        std::filesystem::remove(snap, ec);
    }
};

static forgekv::KeyValueStore make_store(const std::string& path) {
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(path);
    return forgekv::KeyValueStore(std::move(storage), std::move(wal));
}

static std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), {}};
}

static void write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

// =============================================================================
// SH1. Empty store snapshot → load → 0 records
// =============================================================================
TEST(sh1_empty_store_snapshot) {
    SnapFiles sf("sh1");
    {
        auto store = make_store(sf.wal);
        ASSERT_TRUE(store.empty());
        ASSERT_TRUE(store.snapshot());
    }
    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_FALSE(result.corrupt);
    ASSERT_EQ(result.data.records.size(), std::size_t{0});
}

// =============================================================================
// SH2. Large snapshot (200 keys) → restart → all correct
// =============================================================================
TEST(sh2_large_snapshot_restart) {
    SnapFiles sf("sh2");
    const int N = 200;
    {
        auto store = make_store(sf.wal);
        for (int i = 0; i < N; ++i) {
            store.set("bigkey" + std::to_string(i), "bigval" + std::to_string(i));
        }
        ASSERT_TRUE(store.snapshot());
    }
    {
        auto store = make_store(sf.wal);
        ASSERT_EQ(store.size(), static_cast<std::size_t>(N));
        for (int i = 0; i < N; ++i) {
            auto v = store.get("bigkey" + std::to_string(i));
            ASSERT_HAS_VALUE(v);
            ASSERT_EQ(*v, "bigval" + std::to_string(i));
        }
    }
}

// =============================================================================
// SH3. Permanent keys in snapshot survive restart
// =============================================================================
TEST(sh3_permanent_keys_survive_snapshot) {
    SnapFiles sf("sh3");
    {
        auto store = make_store(sf.wal);
        store.set("perm1", "forever1");
        store.set("perm2", "forever2");
        ASSERT_TRUE(store.snapshot());
    }
    {
        auto store = make_store(sf.wal);
        ASSERT_EQ(*store.get("perm1"), "forever1");
        ASSERT_EQ(*store.get("perm2"), "forever2");
    }
}

// =============================================================================
// SH4. TTL keys (future expiry) in snapshot survive restart
// =============================================================================
TEST(sh4_ttl_keys_survive_snapshot) {
    SnapFiles sf("sh4");
    {
        auto store = make_store(sf.wal);
        store.set_with_ttl("ttl_key", "ttl_val", 3600.0);
        ASSERT_TRUE(store.snapshot());
    }
    {
        auto store = make_store(sf.wal);
        ASSERT_TRUE(store.exists("ttl_key"));
        ASSERT_EQ(*store.get("ttl_key"), "ttl_val");
        // TTL should still show remaining time.
        const double remaining = store.ttl("ttl_key");
        ASSERT_TRUE(remaining > 0.0);
    }
}

// =============================================================================
// SH5. Expired keys are NOT included in snapshot
// =============================================================================
TEST(sh5_expired_keys_excluded_from_snapshot) {
    SnapFiles sf("sh5");
    {
        auto store = make_store(sf.wal);
        store.set("perm",    "stays");
        store.set_with_ttl("expires", "gone", 0.001); // 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        store.run_cleanup_now(); // force expire pass
        ASSERT_TRUE(store.snapshot());
    }
    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_FALSE(result.corrupt);
    // Only "perm" should be in the snapshot.
    ASSERT_EQ(result.data.records.size(), std::size_t{1});
    ASSERT_EQ(result.data.records[0].first, "perm");
}

// =============================================================================
// SH6. Corrupt snapshot magic → recovery falls back to WAL → correct state
// =============================================================================
TEST(sh6_corrupt_snapshot_wal_fallback) {
    SnapFiles sf("sh6");
    {
        auto store = make_store(sf.wal);
        store.set("key1", "val1");
        store.set("key2", "val2");
        ASSERT_TRUE(store.snapshot());
    }
    // Corrupt the snapshot magic.
    auto snap_bytes = read_bytes(sf.snap);
    ASSERT_FALSE(snap_bytes.empty());
    snap_bytes[0] ^= 0xFF;
    write_bytes(sf.snap, snap_bytes);

    // Recovery must fall back to WAL — no exception, correct state.
    auto store = make_store(sf.wal);
    ASSERT_EQ(*store.get("key1"), "val1");
    ASSERT_EQ(*store.get("key2"), "val2");
}

// =============================================================================
// SH7. Truncated snapshot → corrupt=true from load()
// =============================================================================
TEST(sh7_truncated_snapshot_corrupt) {
    SnapFiles sf("sh7");
    {
        auto store = make_store(sf.wal);
        store.set("k", "v");
        ASSERT_TRUE(store.snapshot());
    }
    auto snap_bytes = read_bytes(sf.snap);
    ASSERT_TRUE(snap_bytes.size() > 8u);
    snap_bytes.resize(8); // truncate to far less than minimum valid header
    write_bytes(sf.snap, snap_bytes);

    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_TRUE(result.corrupt);
}

// =============================================================================
// SH8. Invalid snapshot version byte → corrupt=true from load()
// =============================================================================
TEST(sh8_invalid_snapshot_version_corrupt) {
    SnapFiles sf("sh8");
    {
        auto store = make_store(sf.wal);
        store.set("k", "v");
        ASSERT_TRUE(store.snapshot());
    }
    auto snap_bytes = read_bytes(sf.snap);
    ASSERT_TRUE(snap_bytes.size() >= 5u);
    snap_bytes[4] = 0xFF; // corrupt version byte
    write_bytes(sf.snap, snap_bytes);

    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_TRUE(result.corrupt);
}

// =============================================================================
// SH9. CRC mismatch in snapshot → corrupt=true from load()
// =============================================================================
TEST(sh9_crc_mismatch_corrupt) {
    SnapFiles sf("sh9");
    {
        auto store = make_store(sf.wal);
        store.set("integrity", "check");
        ASSERT_TRUE(store.snapshot());
    }
    // Flip the last 4 bytes (CRC field).
    auto snap_bytes = read_bytes(sf.snap);
    ASSERT_TRUE(snap_bytes.size() >= 4u);
    snap_bytes[snap_bytes.size()-1] ^= 0xFF;
    snap_bytes[snap_bytes.size()-2] ^= 0xFF;
    write_bytes(sf.snap, snap_bytes);

    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_TRUE(result.corrupt);
}

// =============================================================================
// SH10. Corrupt snapshot + valid WAL → recovery succeeds from WAL alone
// =============================================================================
TEST(sh10_corrupt_snapshot_wal_only_recovery) {
    SnapFiles sf("sh10");
    {
        auto store = make_store(sf.wal);
        store.set("a", "alpha");
        store.set("b", "beta");
        ASSERT_TRUE(store.snapshot());
        store.set("c", "gamma"); // post-snapshot WAL
    }
    // Corrupt the entire snapshot file.
    write_bytes(sf.snap, std::vector<uint8_t>(10, 0xFF));

    // Recovery must fall back to full WAL replay.
    auto store = make_store(sf.wal);
    ASSERT_EQ(*store.get("a"), "alpha");
    ASSERT_EQ(*store.get("b"), "beta");
    ASSERT_EQ(*store.get("c"), "gamma");
    ASSERT_EQ(store.size(), std::size_t{3});
}

// =============================================================================
// SH11. Missing snapshot file → exists=false, recovery uses full WAL
// =============================================================================
TEST(sh11_missing_snapshot_wal_only) {
    SnapFiles sf("sh11");
    std::error_code ec;
    std::filesystem::remove(sf.snap, ec);
    {
        auto store = make_store(sf.wal);
        store.set("only_wal", "value");
    }
    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_FALSE(result.exists);

    auto store = make_store(sf.wal);
    ASSERT_EQ(*store.get("only_wal"), "value");
}

// =============================================================================
// SH12. Snapshot replacement: second snapshot() overwrites first
// =============================================================================
TEST(sh12_snapshot_replacement) {
    SnapFiles sf("sh12");
    {
        auto store = make_store(sf.wal);
        store.set("x", "1");
        ASSERT_TRUE(store.snapshot()); // snapshot1
        store.set("y", "2");
        ASSERT_TRUE(store.snapshot()); // snapshot2 overwrites snapshot1
    }
    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_FALSE(result.corrupt);
    // snapshot2 must contain both x=1 and y=2
    ASSERT_EQ(result.data.records.size(), std::size_t{2});
}

// =============================================================================
// SH13. Snapshot then additional writes → restart → snapshot + WAL tail
// =============================================================================
TEST(sh13_snapshot_then_writes_restart) {
    SnapFiles sf("sh13");
    {
        auto store = make_store(sf.wal);
        store.set("snap",  "in_snapshot");
        ASSERT_TRUE(store.snapshot());
        store.set("post",  "in_wal_tail");
        store.del("snap");  // delete key that was in snapshot
    }
    {
        auto store = make_store(sf.wal);
        ASSERT_FALSE(store.exists("snap")); // deleted in WAL tail
        ASSERT_EQ(*store.get("post"), "in_wal_tail");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// =============================================================================
// SH14. SnapshotManager::remove() on existing file → file gone
// =============================================================================
TEST(sh14_snapshot_manager_remove_existing) {
    SnapFiles sf("sh14");
    {
        auto store = make_store(sf.wal);
        store.set("k", "v");
        ASSERT_TRUE(store.snapshot());
    }
    ASSERT_TRUE(std::filesystem::exists(sf.snap));
    forgekv::SnapshotManager sm(sf.wal);
    ASSERT_TRUE(sm.remove());
    ASSERT_FALSE(std::filesystem::exists(sf.snap));
}

// =============================================================================
// SH15. SnapshotManager::remove() on nonexistent file → returns true
// =============================================================================
TEST(sh15_snapshot_manager_remove_nonexistent) {
    SnapFiles sf("sh15");
    std::error_code ec;
    std::filesystem::remove(sf.snap, ec);
    forgekv::SnapshotManager sm(sf.wal);
    ASSERT_TRUE(sm.remove()); // safe no-op
}

// =============================================================================
// SH16. Snapshot + compact() → snapshot deleted → restart uses compacted WAL
// =============================================================================
TEST(sh16_compact_removes_snapshot) {
    SnapFiles sf("sh16");
    {
        auto store = make_store(sf.wal);
        store.set("a", "1");
        store.set("b", "2");
        ASSERT_TRUE(store.snapshot());
        ASSERT_TRUE(std::filesystem::exists(sf.snap));
        store.compact();
        ASSERT_FALSE(std::filesystem::exists(sf.snap));
    }
    {
        auto store = make_store(sf.wal);
        ASSERT_EQ(*store.get("a"), "1");
        ASSERT_EQ(*store.get("b"), "2");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// SH17. Snapshot wal_offset excludes pre-snapshot records from re-replay
// =============================================================================
TEST(sh17_snapshot_wal_offset_correctness) {
    SnapFiles sf("sh17");
    {
        auto store = make_store(sf.wal);
        store.set("before", "v1"); // pre-snapshot
        ASSERT_TRUE(store.snapshot());
        store.set("before", "v2"); // post-snapshot WAL tail
    }
    {
        auto store = make_store(sf.wal);
        // "before" should have the value from the WAL tail (v2), not v1 again.
        ASSERT_EQ(*store.get("before"), "v2");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// =============================================================================
// SH18. Truncated record payload in snapshot → corrupt=true from load()
// =============================================================================
TEST(sh18_truncated_record_in_snapshot) {
    SnapFiles sf("sh18");
    {
        auto store = make_store(sf.wal);
        store.set("longkey_aaaaa", "longval_bbbbb");
        ASSERT_TRUE(store.snapshot());
    }
    // Truncate the file to cut into a record (remove last ~20 bytes + CRC).
    auto snap_bytes = read_bytes(sf.snap);
    ASSERT_TRUE(snap_bytes.size() > 25u);
    snap_bytes.resize(snap_bytes.size() - 20);
    write_bytes(sf.snap, snap_bytes);

    forgekv::SnapshotManager sm(sf.wal);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_TRUE(result.corrupt);
}

// =============================================================================
// SH19. Snapshot with TTL keys: recovery restores them with expiry
// =============================================================================
TEST(sh19_snapshot_ttl_recovery_restores_expiry) {
    SnapFiles sf("sh19");
    {
        auto store = make_store(sf.wal);
        store.set_with_ttl("expiring", "val", 7200.0); // 2-hour TTL
        ASSERT_TRUE(store.snapshot());
    }
    {
        auto store = make_store(sf.wal);
        // Key still present after recovery.
        ASSERT_TRUE(store.exists("expiring"));
        // TTL should still be a positive value (not permanent).
        const double remaining = store.ttl("expiring");
        ASSERT_TRUE(remaining > 0.0);
        ASSERT_TRUE(remaining != forgekv::kTtlPermanent);
    }
}

// =============================================================================
// SH20. last_snapshot_time_us is nonzero after successful snapshot
// =============================================================================
TEST(sh20_stats_last_snapshot_time_after_snapshot) {
    SnapFiles sf("sh20");
    auto store = make_store(sf.wal);
    ASSERT_EQ(store.stats().last_snapshot_time_us, std::uint64_t{0});
    store.set("k", "v");
    ASSERT_TRUE(store.snapshot());
    ASSERT_TRUE(store.stats().last_snapshot_time_us > 0u);
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Snapshot Hardening Tests\n";
    std::cout << std::string(46, '=') << "\n\n";
    for (const auto& tc : tests) {
        std::cout << "  [ RUN  ] " << tc.name << "\n";
        try {
            tc.fn();
            std::cout << "  [ PASS ] " << tc.name << "\n";
            ++passed;
        } catch (const AssertionFailure& e) {
            std::cout << "  [ FAIL ] " << tc.name << "\n";
            std::cout << "           " << e.message << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "  [ FAIL ] " << tc.name << " (unexpected exception)\n";
            std::cout << "           " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "  [ FAIL ] " << tc.name << " (unknown exception)\n";
            ++failed;
        }
    }
    std::cout << "\n" << std::string(46, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
