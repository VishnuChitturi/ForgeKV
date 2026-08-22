// =============================================================================
// ForgeKV — Stage 13: TTL Hardening Tests
// =============================================================================
//
// Tests TTL boundary cases, edge conditions, and interactions with
// persistence. Avoids fragile sleeps where possible; uses run_cleanup_now()
// for synchronous expiry passes. Where real-time waits are unavoidable (to
// actually expire a key), a minimal documented sleep is used.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

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

struct TempFiles {
    std::string wal;
    std::string snap;
    explicit TempFiles(const std::string& name)
        : wal("test_th_" + name + ".wal")
        , snap("test_th_" + name + ".wal.snapshot") {}
    ~TempFiles() {
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

// Wait up to timeout for cond() to become true. Returns true if it did.
static bool wait_until(std::function<bool()> cond,
                       std::chrono::milliseconds timeout =
                           std::chrono::milliseconds{500}) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!cond()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return true;
}

// =============================================================================
// TH1. Permanent SET has kTtlPermanent TTL.
// =============================================================================
TEST(th1_permanent_set_ttl_permanent) {
    TempFiles tf("th1");
    auto store = make_store(tf.wal);
    store.set("perm", "value");
    ASSERT_EQ(store.ttl("perm"), forgekv::kTtlPermanent);
}

// =============================================================================
// TH2. set_with_ttl with positive TTL: key present, ttl() returns > 0.
// =============================================================================
TEST(th2_positive_ttl_key_present) {
    TempFiles tf("th2");
    auto store = make_store(tf.wal);
    store.set_with_ttl("limited", "value", 3600.0);
    ASSERT_TRUE(store.exists("limited"));
    const double remaining = store.ttl("limited");
    ASSERT_TRUE(remaining > 0.0);
    ASSERT_TRUE(remaining < 3601.0);
    ASSERT_TRUE(remaining != forgekv::kTtlPermanent);
}

// =============================================================================
// TH3. Very small TTL (1ms): key expires quickly.
// Uses minimal real-time wait since expiration requires wall-clock advance.
// =============================================================================
TEST(th3_small_ttl_expires) {
    TempFiles tf("th3");
    auto store = make_store(tf.wal);
    store.set_with_ttl("fast", "gone", 0.001); // 1ms

    // Wait for expiry, then force cleanup.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    ASSERT_FALSE(store.exists("fast"));
    ASSERT_NO_VALUE(store.get("fast"));
    ASSERT_EQ(store.ttl("fast"), forgekv::kTtlNotFound);
}

// =============================================================================
// TH4. TTL query immediately after setting: remaining is close to TTL.
// =============================================================================
TEST(th4_ttl_query_immediately_after_set) {
    TempFiles tf("th4");
    auto store = make_store(tf.wal);
    store.set_with_ttl("key", "val", 100.0);
    const double remaining = store.ttl("key");
    // Should be positive and not more than 100 seconds.
    ASSERT_TRUE(remaining > 0.0);
    ASSERT_TRUE(remaining <= 100.0);
}

// =============================================================================
// TH5. Normal set() after set_with_ttl() removes TTL (key becomes permanent).
// =============================================================================
TEST(th5_set_removes_ttl) {
    TempFiles tf("th5");
    auto store = make_store(tf.wal);
    store.set_with_ttl("key", "expiring", 0.001);
    store.set("key", "permanent"); // overwrite with permanent

    // Even after the original TTL would have expired, key stays.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    ASSERT_TRUE(store.exists("key"));
    ASSERT_EQ(*store.get("key"), "permanent");
    ASSERT_EQ(store.ttl("key"), forgekv::kTtlPermanent);
}

// =============================================================================
// TH6. TTL query on missing key returns kTtlNotFound.
// =============================================================================
TEST(th6_ttl_missing_key_not_found) {
    TempFiles tf("th6");
    auto store = make_store(tf.wal);
    ASSERT_EQ(store.ttl("nonexistent"), forgekv::kTtlNotFound);
}

// =============================================================================
// TH7. TTL query on expired key (not yet cleaned up) returns kTtlNotFound.
// =============================================================================
TEST(th7_ttl_expired_key_not_found) {
    TempFiles tf("th7");
    auto store = make_store(tf.wal);
    store.set_with_ttl("expired", "val", 0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    // Don't call run_cleanup_now(): key still in map but expired.
    ASSERT_EQ(store.ttl("expired"), forgekv::kTtlNotFound);
}

// =============================================================================
// TH8. get() on expired key returns nullopt.
// =============================================================================
TEST(th8_get_expired_key_nullopt) {
    TempFiles tf("th8");
    auto store = make_store(tf.wal);
    store.set_with_ttl("exp", "val", 0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_NO_VALUE(store.get("exp"));
}

// =============================================================================
// TH9. exists() on expired key returns false.
// =============================================================================
TEST(th9_exists_expired_key_false) {
    TempFiles tf("th9");
    auto store = make_store(tf.wal);
    store.set_with_ttl("exp", "val", 0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_FALSE(store.exists("exp"));
}

// =============================================================================
// TH10. del() on expired key returns false (expired == not found).
// =============================================================================
TEST(th10_del_expired_key_returns_false) {
    TempFiles tf("th10");
    auto store = make_store(tf.wal);
    store.set_with_ttl("exp", "val", 0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_FALSE(store.del("exp")); // expired == not found
}

// =============================================================================
// TH11. Expired key followed by SET creates a fresh permanent key.
// =============================================================================
TEST(th11_expired_key_then_set) {
    TempFiles tf("th11");
    auto store = make_store(tf.wal);
    store.set_with_ttl("key", "old", 0.001);
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.set("key", "new");
    ASSERT_TRUE(store.exists("key"));
    ASSERT_EQ(*store.get("key"), "new");
    ASSERT_EQ(store.ttl("key"), forgekv::kTtlPermanent);
}

// =============================================================================
// TH12. set_with_ttl(key, val, 0.0) does NOT store the key.
// =============================================================================
TEST(th12_zero_ttl_not_stored) {
    TempFiles tf("th12");
    auto store = make_store(tf.wal);
    store.set_with_ttl("zero", "val", 0.0);
    ASSERT_FALSE(store.exists("zero"));
    ASSERT_NO_VALUE(store.get("zero"));
    ASSERT_TRUE(store.empty());
    // ttl_set_count must be 0 (key was not stored).
    ASSERT_EQ(store.stats().ttl_set_count, std::uint64_t{0});
}

// =============================================================================
// TH13. Negative TTL does NOT store the key.
// =============================================================================
TEST(th13_negative_ttl_not_stored) {
    TempFiles tf("th13");
    auto store = make_store(tf.wal);
    store.set_with_ttl("neg", "val", -1.0);
    ASSERT_FALSE(store.exists("neg"));
    ASSERT_TRUE(store.empty());
}

// =============================================================================
// TH14. Multiple TTL keys with different deadlines expire independently.
// =============================================================================
TEST(th14_multiple_ttl_different_deadlines) {
    TempFiles tf("th14");
    auto store = make_store(tf.wal);
    store.set_with_ttl("fast", "gone", 0.001);  // expires very soon
    store.set_with_ttl("slow", "here", 3600.0); // far future

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    ASSERT_FALSE(store.exists("fast"));
    ASSERT_TRUE(store.exists("slow"));
    ASSERT_EQ(store.size(), std::size_t{1});
}

// =============================================================================
// TH15. Permanent and TTL keys coexist; permanent survives expiry pass.
// =============================================================================
TEST(th15_permanent_and_ttl_coexist) {
    TempFiles tf("th15");
    auto store = make_store(tf.wal);
    store.set("perm", "forever");
    store.set_with_ttl("temp", "limited", 0.001);

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    ASSERT_TRUE(store.exists("perm"));
    ASSERT_FALSE(store.exists("temp"));
    ASSERT_EQ(store.size(), std::size_t{1});
}

// =============================================================================
// TH16. Restart before expiration: key persists through WAL recovery.
// =============================================================================
TEST(th16_restart_before_expiry_key_persists) {
    TempFiles tf("th16");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("future", "val", 3600.0);
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists("future"));
        ASSERT_EQ(*store.get("future"), "val");
    }
}

// =============================================================================
// TH17. Restart after expiration: already-expired key not recovered.
// =============================================================================
TEST(th17_restart_after_expiry_key_absent) {
    TempFiles tf("th17");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("shortlived", "gone", 0.001);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    {
        // Recovery skips kOpSetWithExpiry records whose expiry has passed.
        auto store = make_store(tf.wal);
        ASSERT_FALSE(store.exists("shortlived"));
        ASSERT_TRUE(store.empty());
    }
}

// =============================================================================
// TH18. Snapshot + TTL: TTL key in snapshot survives restart.
// =============================================================================
TEST(th18_snapshot_ttl_survives_restart) {
    TempFiles tf("th18");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("snap_ttl", "val", 3600.0);
        ASSERT_TRUE(store.snapshot());
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists("snap_ttl"));
        ASSERT_EQ(*store.get("snap_ttl"), "val");
        ASSERT_TRUE(store.ttl("snap_ttl") > 0.0);
    }
}

// =============================================================================
// TH19. Compaction preserves TTL metadata for live keys.
// =============================================================================
TEST(th19_compact_preserves_ttl_metadata) {
    TempFiles tf("th19");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("expiring", "val", 3600.0);
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists("expiring"));
        // Must still have a positive remaining TTL (not become permanent).
        const double remaining = store.ttl("expiring");
        ASSERT_TRUE(remaining > 0.0);
        ASSERT_TRUE(remaining != forgekv::kTtlPermanent);
    }
}

// =============================================================================
// TH20. Compaction excludes expired TTL keys.
// =============================================================================
TEST(th20_compact_excludes_expired_keys) {
    TempFiles tf("th20");
    auto store = make_store(tf.wal);
    store.set("perm", "stays");
    store.set_with_ttl("exp", "gone", 0.001);

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();
    store.compact();

    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_TRUE(store.exists("perm"));
    ASSERT_FALSE(store.exists("exp"));

    // Restart from compacted WAL.
    {
        auto s2 = make_store(tf.wal);
        ASSERT_EQ(s2.size(), std::size_t{1});
        ASSERT_EQ(*s2.get("perm"), "stays");
        ASSERT_FALSE(s2.exists("exp"));
    }
}

// =============================================================================
// TH21. size() and empty() respect TTL (expired keys not counted).
// =============================================================================
TEST(th21_size_empty_respect_ttl) {
    TempFiles tf("th21");
    auto store = make_store(tf.wal);
    store.set("perm", "v");
    store.set_with_ttl("exp", "v", 0.001);
    ASSERT_EQ(store.size(), std::size_t{2});
    ASSERT_FALSE(store.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_FALSE(store.empty());
}

// =============================================================================
// TH22. Updating TTL with another set_with_ttl replaces the expiry.
// =============================================================================
TEST(th22_update_ttl_replaces_expiry) {
    TempFiles tf("th22");
    auto store = make_store(tf.wal);
    store.set_with_ttl("key", "v1", 0.001); // very short
    store.set_with_ttl("key", "v2", 3600.0); // long-lived replacement

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    // The original short TTL is gone; the replacement should still be there.
    store.run_cleanup_now();

    ASSERT_TRUE(store.exists("key"));
    ASSERT_EQ(*store.get("key"), "v2");
    ASSERT_TRUE(store.ttl("key") > 0.0);
}

// =============================================================================
// TH23. expired_count stat reflects keys removed by background cleanup.
// =============================================================================
TEST(th23_expired_count_stat) {
    TempFiles tf("th23");
    auto store = make_store(tf.wal);
    store.set_with_ttl("exp1", "v", 0.001);
    store.set_with_ttl("exp2", "v", 0.001);
    store.set("perm", "v");

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    const auto s = store.stats();
    ASSERT_EQ(s.expired_count, std::uint64_t{2});
    ASSERT_EQ(s.delete_count,  std::uint64_t{0}); // expirations != explicit deletes
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — TTL Hardening Tests\n";
    std::cout << std::string(40, '=') << "\n\n";
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
    std::cout << "\n" << std::string(40, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
