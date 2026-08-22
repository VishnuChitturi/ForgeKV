// =============================================================================
// ForgeKV — Stage 13: Compaction Robustness Tests
// =============================================================================
//
// Tests compaction edge cases: state invariants, TTL interaction, restart
// after compaction, and combinations with snapshots.
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
#include <unordered_map>
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
        : wal("test_cr_" + name + ".wal")
        , snap("test_cr_" + name + ".wal.snapshot") {}
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

// Capture the current logical state as a sorted map for comparison.
static std::vector<std::pair<std::string,std::string>>
capture_state(forgekv::KeyValueStore& store, const std::vector<std::string>& keys) {
    std::vector<std::pair<std::string,std::string>> state;
    for (const auto& k : keys) {
        auto v = store.get(k);
        if (v.has_value()) {
            state.emplace_back(k, *v);
        }
    }
    std::sort(state.begin(), state.end());
    return state;
}

// =============================================================================
// CR1. Repeated SET of same key: compact produces exactly one record.
// =============================================================================
TEST(cr1_repeated_set_compacts_to_one) {
    TempFiles tf("cr1");
    auto store = make_store(tf.wal);
    for (int i = 0; i < 50; ++i) {
        store.set("counter", "v" + std::to_string(i));
    }
    ASSERT_EQ(*store.get("counter"), "v49");

    store.compact();

    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_EQ(*store.get("counter"), "v49");

    // Verify WAL has exactly one record.
    forgekv::WAL wal(tf.wal);
    std::size_t rec_count = 0;
    (void)wal.replay([&](const forgekv::WalRecord&) { ++rec_count; });
    ASSERT_EQ(rec_count, std::size_t{1});
}

// =============================================================================
// CR2. SET → DELETE: compact produces empty WAL.
// =============================================================================
TEST(cr2_set_delete_compact_empty) {
    TempFiles tf("cr2");
    auto store = make_store(tf.wal);
    store.set("key", "val");
    store.del("key");
    ASSERT_TRUE(store.empty());

    store.compact();

    ASSERT_TRUE(store.empty());

    forgekv::WAL wal(tf.wal);
    std::size_t rec_count = 0;
    (void)wal.replay([&](const forgekv::WalRecord&) { ++rec_count; });
    ASSERT_EQ(rec_count, std::size_t{0});
}

// =============================================================================
// CR3. SET → DELETE → SET: compact produces one record with final value.
// =============================================================================
TEST(cr3_set_del_set_compact) {
    TempFiles tf("cr3");
    auto store = make_store(tf.wal);
    store.set("key", "first");
    store.del("key");
    store.set("key", "third");

    store.compact();

    ASSERT_EQ(*store.get("key"), "third");
    ASSERT_EQ(store.size(), std::size_t{1});

    // Restart from compacted WAL.
    {
        auto s2 = make_store(tf.wal);
        ASSERT_EQ(*s2.get("key"), "third");
    }
}

// =============================================================================
// CR4. Many keys with multiple updates: state before == state after compact.
// =============================================================================
TEST(cr4_many_keys_state_preserved) {
    TempFiles tf("cr4");
    auto store = make_store(tf.wal);

    const int N = 100;
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back("k" + std::to_string(i));
    }

    // Write multiple versions.
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < N; ++i) {
            store.set(keys[i], "r" + std::to_string(round) + "_" + std::to_string(i));
        }
    }
    // Delete every 10th key.
    for (int i = 0; i < N; i += 10) {
        store.del(keys[i]);
    }

    auto state_before = capture_state(store, keys);
    const std::size_t count_before = store.size();

    store.compact();

    auto state_after = capture_state(store, keys);
    ASSERT_EQ(state_before, state_after);
    ASSERT_EQ(store.size(), count_before);
}

// =============================================================================
// CR5. Permanent keys survive compaction + restart.
// =============================================================================
TEST(cr5_permanent_keys_survive_compact_restart) {
    TempFiles tf("cr5");
    {
        auto store = make_store(tf.wal);
        store.set("perm1", "alpha");
        store.set("perm2", "beta");
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("perm1"), "alpha");
        ASSERT_EQ(*store.get("perm2"), "beta");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// CR6. TTL keys with future expiry survive compaction + restart.
// =============================================================================
TEST(cr6_ttl_keys_survive_compact_restart) {
    TempFiles tf("cr6");
    {
        auto store = make_store(tf.wal);
        store.set_with_ttl("future", "alive", 3600.0);
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists("future"));
        ASSERT_EQ(*store.get("future"), "alive");
    }
}

// =============================================================================
// CR7. Expired TTL keys are excluded from compact output.
// =============================================================================
TEST(cr7_expired_keys_excluded_from_compact) {
    TempFiles tf("cr7");
    auto store = make_store(tf.wal);
    store.set("perm", "stays");
    store.set_with_ttl("expires", "gone", 0.001); // 1ms TTL

    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    store.run_cleanup_now();

    store.compact();

    // Only "perm" should remain.
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_TRUE(store.exists("perm"));
    ASSERT_FALSE(store.exists("expires"));

    // Verify WAL has exactly one record.
    forgekv::WAL wal(tf.wal);
    std::size_t rec_count = 0;
    (void)wal.replay([&](const forgekv::WalRecord&) { ++rec_count; });
    ASSERT_EQ(rec_count, std::size_t{1});
}

// =============================================================================
// CR8. Compact on empty store produces zero-record WAL + correct restart.
// =============================================================================
TEST(cr8_compact_empty_store) {
    TempFiles tf("cr8");
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.empty());
        store.compact();
        ASSERT_TRUE(store.empty());
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.empty());
    }
}

// =============================================================================
// CR9. Compact followed by snapshot: snapshot points into compacted WAL.
// =============================================================================
TEST(cr9_compact_then_snapshot) {
    TempFiles tf("cr9");
    {
        auto store = make_store(tf.wal);
        store.set("a", "1");
        store.set("b", "2");
        store.compact();
        ASSERT_TRUE(store.snapshot());
        store.set("c", "3"); // post-snapshot WAL tail
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("a"), "1");
        ASSERT_EQ(*store.get("b"), "2");
        ASSERT_EQ(*store.get("c"), "3");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// =============================================================================
// CR10. Snapshot followed by compact: compact deletes snapshot.
// =============================================================================
TEST(cr10_snapshot_then_compact) {
    TempFiles tf("cr10");
    {
        auto store = make_store(tf.wal);
        store.set("x", "1");
        ASSERT_TRUE(store.snapshot());
        ASSERT_TRUE(std::filesystem::exists(tf.snap));
        store.compact();
        ASSERT_FALSE(std::filesystem::exists(tf.snap));
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("x"), "1");
    }
}

// =============================================================================
// CR11. Double compact is idempotent: same state, same WAL record count.
// =============================================================================
TEST(cr11_double_compact_idempotent) {
    TempFiles tf("cr11");
    auto store = make_store(tf.wal);
    store.set("p", "1");
    store.set("q", "2");

    store.compact();
    const std::size_t size1 = std::filesystem::file_size(tf.wal);
    const std::size_t count1 = store.size();

    store.compact();
    const std::size_t size2 = std::filesystem::file_size(tf.wal);
    const std::size_t count2 = store.size();

    ASSERT_EQ(size1, size2);
    ASSERT_EQ(count1, count2);
    ASSERT_EQ(*store.get("p"), "1");
    ASSERT_EQ(*store.get("q"), "2");
}

// =============================================================================
// CR12. Compact → write more → compact again → restart: all correct.
// =============================================================================
TEST(cr12_compact_write_compact_restart) {
    TempFiles tf("cr12");
    {
        auto store = make_store(tf.wal);
        store.set("a", "1");
        store.set("b", "2");
        store.compact();

        store.set("c", "3");
        store.del("b");
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("a"), "1");
        ASSERT_FALSE(store.exists("b"));
        ASSERT_EQ(*store.get("c"), "3");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// =============================================================================
// CR13. Compact preserves mixed permanent + TTL keys correctly.
// =============================================================================
TEST(cr13_compact_mixed_permanent_and_ttl) {
    TempFiles tf("cr13");
    {
        auto store = make_store(tf.wal);
        store.set("perm", "forever");
        store.set_with_ttl("ttl", "limited", 3600.0);
        store.compact();
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_EQ(*store.get("perm"), "forever");
        // Permanent key should have kTtlPermanent.
        ASSERT_EQ(store.ttl("perm"), forgekv::kTtlPermanent);
        ASSERT_TRUE(store.exists("ttl"));
        // TTL key should have positive remaining time.
        ASSERT_TRUE(store.ttl("ttl") > 0.0);
    }
}

// =============================================================================
// CR14. WAL size decreases after compaction of redundant records.
// =============================================================================
TEST(cr14_wal_size_decreases_after_compact) {
    TempFiles tf("cr14");
    auto store = make_store(tf.wal);

    // Write 200 updates to 10 keys (a lot of redundant history).
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < 10; ++i) {
            store.set("k" + std::to_string(i), "round" + std::to_string(round));
        }
    }

    const std::uint64_t size_before = store.stats().wal_size_bytes;
    store.compact();
    const std::uint64_t size_after = store.stats().wal_size_bytes;

    ASSERT_TRUE(size_after < size_before);
    ASSERT_EQ(store.size(), std::size_t{10});
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Compaction Robustness Tests\n";
    std::cout << std::string(49, '=') << "\n\n";
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
    std::cout << "\n" << std::string(49, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
