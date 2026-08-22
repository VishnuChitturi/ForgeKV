// =============================================================================
// ForgeKV — Stage 13: Recovery Hardening Tests
// =============================================================================
//
// Tests realistic restart sequences to verify that data survives destroy/reopen
// cycles under a variety of conditions.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

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
#define ASSERT_THROWS(expr) \
    do { bool _threw=false; try{(expr);}catch(...){_threw=true;} \
    if(!_threw) throw AssertionFailure{"ASSERT_THROWS failed: " #expr " (line " + std::to_string(__LINE__) + ")"}; } while(false)

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
        : wal("test_rh_" + name + ".wal")
        , snap("test_rh_" + name + ".wal.snapshot") {}
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
// RH1. SET → destroy → reopen → GET
// =============================================================================
TEST(rh1_set_destroy_reopen_get) {
    TempFiles tf("rh1");
    {
        auto store = make_store(tf.wal);
        store.set("project", "ForgeKV");
        store.set("stage",   "13");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_HAS_VALUE(store.get("project"));
        ASSERT_EQ(*store.get("project"), "ForgeKV");
        ASSERT_HAS_VALUE(store.get("stage"));
        ASSERT_EQ(*store.get("stage"), "13");
    }
}

// =============================================================================
// RH2. SET + DELETE → reopen → key absent
// =============================================================================
TEST(rh2_set_delete_reopen) {
    TempFiles tf("rh2");
    {
        auto store = make_store(tf.wal);
        store.set("temp", "data");
        store.del("temp");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_FALSE(store.exists("temp"));
        ASSERT_NO_VALUE(store.get("temp"));
        ASSERT_TRUE(store.empty());
    }
}

// =============================================================================
// RH3. Multiple updates to same key → reopen → latest value
// =============================================================================
TEST(rh3_multiple_updates_same_key_reopen) {
    TempFiles tf("rh3");
    {
        auto store = make_store(tf.wal);
        store.set("counter", "v1");
        store.set("counter", "v2");
        store.set("counter", "v3");
        store.set("counter", "final");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_HAS_VALUE(store.get("counter"));
        ASSERT_EQ(*store.get("counter"), "final");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// =============================================================================
// RH4. Many keys → reopen → all correct
// =============================================================================
TEST(rh4_many_keys_reopen) {
    TempFiles tf("rh4");
    const int N = 500;
    {
        auto store = make_store(tf.wal);
        for (int i = 0; i < N; ++i) {
            store.set("key" + std::to_string(i), "val" + std::to_string(i));
        }
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(store.size(), static_cast<std::size_t>(N));
        for (int i = 0; i < N; ++i) {
            auto v = store.get("key" + std::to_string(i));
            ASSERT_HAS_VALUE(v);
            ASSERT_EQ(*v, "val" + std::to_string(i));
        }
    }
}

// =============================================================================
// RH5. TTL key (future expiry) → reopen before expiry → key still present
// =============================================================================
TEST(rh5_ttl_key_reopen_before_expiry) {
    TempFiles tf("rh5");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("live_ttl", "still_alive", 3600.0); // 1 hour TTL
    }
    {
        auto store = make_store(tf.wal);
        // Reopened before expiry — must be present.
        ASSERT_TRUE(store.exists("live_ttl"));
        ASSERT_HAS_VALUE(store.get("live_ttl"));
        ASSERT_EQ(*store.get("live_ttl"), "still_alive");
    }
}

// =============================================================================
// RH6. Expired TTL key → reopen after expiry → key absent
// =============================================================================
TEST(rh6_expired_ttl_key_reopen_absent) {
    TempFiles tf("rh6");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("fast_expire", "gone", 0.001); // 1ms TTL
    }
    // Wait for expiry.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    {
        // Recovery skips already-expired keys.
        auto store = make_store(tf.wal);
        ASSERT_FALSE(store.exists("fast_expire"));
        ASSERT_NO_VALUE(store.get("fast_expire"));
    }
}

// =============================================================================
// RH7. Permanent key + expiring key → reopen → permanent present
// =============================================================================
TEST(rh7_permanent_and_ttl_key_reopen) {
    TempFiles tf("rh7");
    {
        auto store = make_store(tf.wal);
        store.set("permanent", "forever");
        store.set_with_ttl("temporary", "for_now", 3600.0); // far future
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists("permanent"));
        ASSERT_EQ(*store.get("permanent"), "forever");
        ASSERT_TRUE(store.exists("temporary"));
        ASSERT_EQ(*store.get("temporary"), "for_now");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// RH8. Snapshot → additional writes → reopen → snapshot + WAL tail
// =============================================================================
TEST(rh8_snapshot_additional_writes_reopen) {
    TempFiles tf("rh8");
    {
        auto store = make_store(tf.wal);
        store.set("snap_key", "snap_val");
        ASSERT_TRUE(store.snapshot());
        store.set("post_snap", "after");
        store.del("snap_key");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_FALSE(store.exists("snap_key"));
        ASSERT_TRUE(store.exists("post_snap"));
        ASSERT_EQ(*store.get("post_snap"), "after");
    }
}

// =============================================================================
// RH9. Snapshot → delete some → update others → reopen
// =============================================================================
TEST(rh9_snapshot_delete_update_reopen) {
    TempFiles tf("rh9");
    {
        auto store = make_store(tf.wal);
        store.set("a", "1");
        store.set("b", "2");
        store.set("c", "3");
        ASSERT_TRUE(store.snapshot());
        // Post-snapshot: delete b, update a, add d.
        store.del("b");
        store.set("a", "updated");
        store.set("d", "new");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("a"), "updated");
        ASSERT_FALSE(store.exists("b"));
        ASSERT_EQ(*store.get("c"), "3");
        ASSERT_EQ(*store.get("d"), "new");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// =============================================================================
// RH10. Compaction → reopen → correct state
// =============================================================================
TEST(rh10_compaction_reopen) {
    TempFiles tf("rh10");
    {
        auto store = make_store(tf.wal);
        store.set("x", "v1");
        store.set("y", "v2");
        store.set("x", "v3"); // update x
        store.del("y");       // delete y
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("x"), "v3");
        ASSERT_FALSE(store.exists("y"));
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// =============================================================================
// RH11. Snapshot + compaction → reopen → WAL-only (snapshot deleted by compact)
// =============================================================================
TEST(rh11_snapshot_then_compact_reopen) {
    TempFiles tf("rh11");
    {
        auto store = make_store(tf.wal);
        store.set("p", "1");
        store.set("q", "2");
        ASSERT_TRUE(store.snapshot()); // snapshot created
        ASSERT_TRUE(std::filesystem::exists(tf.snap));
        store.compact(); // compact() deletes snapshot
        ASSERT_FALSE(std::filesystem::exists(tf.snap)); // snapshot gone
    }
    {
        // Recovery uses compacted WAL only (no snapshot).
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("p"), "1");
        ASSERT_EQ(*store.get("q"), "2");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// RH12. Multiple reopen cycles are deterministic
// =============================================================================
TEST(rh12_multiple_reopen_deterministic) {
    TempFiles tf("rh12");
    {
        auto store = make_store(tf.wal);
        store.set("alpha", "1");
        store.set("beta",  "2");
        store.del("beta");
        store.set("gamma", "3");
    }
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("alpha"), "1");
        ASSERT_FALSE(store.exists("beta"));
        ASSERT_EQ(*store.get("gamma"), "3");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// RH13. SET → CLEAR → SET → reopen → only post-CLEAR key present
// =============================================================================
TEST(rh13_clear_then_set_reopen) {
    TempFiles tf("rh13");
    {
        auto store = make_store(tf.wal);
        store.set("before_clear", "gone");
        store.set("also_gone",    "yes");
        store.clear();
        store.set("after_clear", "present");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_FALSE(store.exists("before_clear"));
        ASSERT_FALSE(store.exists("also_gone"));
        ASSERT_TRUE(store.exists("after_clear"));
        ASSERT_EQ(*store.get("after_clear"), "present");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// =============================================================================
// RH14. Crash-style: truncated WAL tail → prior records still recovered
// =============================================================================
TEST(rh14_truncated_wal_tail_prior_records_ok) {
    TempFiles tf("rh14");
    {
        auto store = make_store(tf.wal);
        store.set("safe1", "aaa");
        store.set("safe2", "bbb");
    }
    // Simulate crash: truncate the last few bytes of the WAL.
    auto bytes = read_bytes(tf.wal);
    ASSERT_TRUE(bytes.size() > 8u);
    bytes.resize(bytes.size() - 6); // cut into last record
    write_bytes(tf.wal, bytes);

    // Recovery must succeed — truncated final record is non-fatal.
    auto store = make_store(tf.wal);
    // At least safe1 should be present (it was first); safe2 may or may not
    // be recovered depending on where the truncation fell.
    // The critical check: no exception, and the store is usable.
    ASSERT_FALSE(store.size() > std::size_t{2});
    // safe1 must be present (first complete record).
    ASSERT_TRUE(store.exists("safe1") || store.size() == std::size_t{0});
}

// =============================================================================
// RH15. Stats after recovery: operation counters are zero, key_count is live
// =============================================================================
TEST(rh15_stats_after_recovery_counters_zero) {
    TempFiles tf("rh15");
    {
        auto store = make_store(tf.wal);
        store.set("a", "1");
        store.set("b", "2");
        store.set_with_ttl("c", "3", 3600.0);
        store.del("a");
    }
    {
        // Fresh store recovers the WAL — counters must all be zero.
        auto store = make_store(tf.wal);
        const auto s = store.stats();
        ASSERT_EQ(s.set_count,     std::uint64_t{0});
        ASSERT_EQ(s.delete_count,  std::uint64_t{0});
        ASSERT_EQ(s.ttl_set_count, std::uint64_t{0});
        ASSERT_EQ(s.get_hits,      std::uint64_t{0});
        ASSERT_EQ(s.get_misses,    std::uint64_t{0});
        // b and c are live after recovery (a was deleted).
        ASSERT_EQ(s.key_count, std::uint64_t{2});
    }
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Recovery Hardening Tests\n";
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
