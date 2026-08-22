// =============================================================================
// ForgeKV — Stage 13: Lifecycle and Resource Behavior Tests
// =============================================================================
//
// Tests that verify correct resource management:
//   - Store construction and destruction (no leaks, no hangs)
//   - Cleanup thread shutdown does not block indefinitely
//   - WAL file handles closed after destruction
//   - Temporary files removed after compaction/snapshot
//   - Server start/stop lifecycle
//   - Repeated create/destroy cycles are stable
// =============================================================================

#define CPPHTTPLIB_THREAD_POOL_COUNT 0

#include "forgekv/kv_store.h"
#include "forgekv/http_server.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"
#include "forgekv/snapshot.h"

#include "httplib.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
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
        : wal("test_lc_" + name + ".wal")
        , snap("test_lc_" + name + ".wal.snapshot") {}
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

// =============================================================================
// LC1. Constructing and destroying a store without doing anything is safe.
// =============================================================================
TEST(lc1_construct_destroy_empty_store) {
    TempFiles tf("lc1");
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.empty());
    }
    // Destruction must complete without hanging.
    ASSERT_TRUE(std::filesystem::exists(tf.wal));
}

// =============================================================================
// LC2. Construct → write → destroy → construct again reopens cleanly.
// =============================================================================
TEST(lc2_reopen_same_wal) {
    TempFiles tf("lc2");
    {
        auto store = make_store(tf.wal);
        store.set("persistent", "value");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_HAS_VALUE(store.get("persistent"));
        ASSERT_EQ(*store.get("persistent"), "value");
    }
}

// =============================================================================
// LC3. Multiple store instances with distinct WAL paths are fully independent.
// =============================================================================
TEST(lc3_independent_stores) {
    TempFiles tf1("lc3a");
    TempFiles tf2("lc3b");

    auto store1 = make_store(tf1.wal);
    auto store2 = make_store(tf2.wal);

    store1.set("k", "store1_val");
    store2.set("k", "store2_val");

    ASSERT_EQ(*store1.get("k"), "store1_val");
    ASSERT_EQ(*store2.get("k"), "store2_val");
    ASSERT_EQ(store1.size(), std::size_t{1});
    ASSERT_EQ(store2.size(), std::size_t{1});
}

// =============================================================================
// LC4. Repeated create/destroy cycles (10x) produce no resource leaks
//      (detectable as no leftover temp files, no exception on each open).
// =============================================================================
TEST(lc4_repeated_create_destroy_cycles) {
    TempFiles tf("lc4");
    for (int cycle = 0; cycle < 10; ++cycle) {
        auto store = make_store(tf.wal);
        store.set("cycle", std::to_string(cycle));
        // Destructor must complete without hanging.
    }
    // Final reopen should see the last written value.
    auto store = make_store(tf.wal);
    ASSERT_HAS_VALUE(store.get("cycle"));
    ASSERT_EQ(*store.get("cycle"), "9");
}

// =============================================================================
// LC5. Destructor joins the cleanup thread (no hang on destruction).
//      Verified by timing: a store with a 1s cleanup interval must destruct
//      well within a few seconds (the cv.notify_all wakes it immediately).
// =============================================================================
TEST(lc5_destructor_joins_cleanup_thread) {
    TempFiles tf("lc5");
    const auto before = std::chrono::steady_clock::now();
    {
        auto store = make_store(tf.wal);
        store.set("k", "v");
        // Destructor runs here. Must signal the cleanup thread and join quickly.
    }
    const auto elapsed = std::chrono::steady_clock::now() - before;
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    // Allow up to 3000ms for any edge case (it should be milliseconds).
    ASSERT_TRUE(elapsed_ms < 3000);
}

// =============================================================================
// LC6. Compact() does not leave temporary files behind.
// =============================================================================
TEST(lc6_compact_no_temp_files) {
    TempFiles tf("lc6");
    auto store = make_store(tf.wal);
    store.set("a", "1");
    store.set("b", "2");
    store.compact();

    // Check that no .tmp or .tmp.* files exist alongside the WAL.
    auto wal_dir_p = std::filesystem::path(tf.wal).parent_path();
    if (wal_dir_p.empty()) wal_dir_p = ".";
    const auto wal_dir  = wal_dir_p;
    const auto wal_stem = std::filesystem::path(tf.wal).filename().string();
    bool found_temp = false;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir)) {
        const std::string fname = entry.path().filename().string();
        if (fname.find(wal_stem) != std::string::npos &&
            fname.find(".tmp") != std::string::npos) {
            found_temp = true;
        }
    }
    ASSERT_FALSE(found_temp);
}

// =============================================================================
// LC7. snapshot() does not leave temporary files behind after success.
// =============================================================================
TEST(lc7_snapshot_no_temp_files_on_success) {
    TempFiles tf("lc7");
    auto store = make_store(tf.wal);
    store.set("k", "v");
    ASSERT_TRUE(store.snapshot());

    auto snap_dir_p = std::filesystem::path(tf.wal).parent_path();
    if (snap_dir_p.empty()) snap_dir_p = ".";
    const auto wal_dir  = snap_dir_p;
    const auto snap_stem = std::filesystem::path(tf.snap).filename().string();
    bool found_temp = false;
    for (const auto& entry : std::filesystem::directory_iterator(wal_dir)) {
        const std::string fname = entry.path().filename().string();
        if (fname.find(snap_stem) != std::string::npos &&
            fname.find(".tmp") != std::string::npos) {
            found_temp = true;
        }
    }
    ASSERT_FALSE(found_temp);
}

// =============================================================================
// LC8. HTTP server starts and stops cleanly.
// =============================================================================
TEST(lc8_http_server_start_stop) {
    TempFiles tf("lc8");
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(tf.wal);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    forgekv::HttpServer server(store);
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_TRUE(port > 0);

    std::thread srv_thread([&server]() { server.listen_after_bind(); });
    server.wait_until_ready();

    // Make one request to confirm the server is running.
    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/health");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);

    server.stop();
    srv_thread.join();
    // If we get here without hanging, the test passes.
}

// =============================================================================
// LC9. Repeated HTTP server create/stop cycles (3x) are stable.
// =============================================================================
TEST(lc9_repeated_server_create_stop) {
    TempFiles tf("lc9");
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(tf.wal);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    for (int cycle = 0; cycle < 3; ++cycle) {
        forgekv::HttpServer server(store);
        const int port = server.bind_to_any_port("127.0.0.1");
        ASSERT_TRUE(port > 0);

        std::thread srv_thread([&server]() { server.listen_after_bind(); });
        server.wait_until_ready();

        httplib::Client cli("127.0.0.1", port);
        auto res = cli.Get("/health");
        ASSERT_TRUE(res != nullptr);
        ASSERT_EQ(res->status, 200);

        server.stop();
        srv_thread.join();
    }
}

// =============================================================================
// LC10. WAL file exists after store destruction (data persisted).
// =============================================================================
TEST(lc10_wal_file_exists_after_destruction) {
    TempFiles tf("lc10");
    {
        auto store = make_store(tf.wal);
        store.set("durability", "test");
    }
    // WAL file must exist on disk after destruction.
    ASSERT_TRUE(std::filesystem::exists(tf.wal));
    ASSERT_TRUE(std::filesystem::file_size(tf.wal) > 0u);
}

// =============================================================================
// LC11. Snapshot file does NOT exist until snapshot() is called.
// =============================================================================
TEST(lc11_snapshot_file_absent_before_snapshot_call) {
    TempFiles tf("lc11");
    {
        auto store = make_store(tf.wal);
        store.set("k", "v");
        ASSERT_FALSE(std::filesystem::exists(tf.snap));
        ASSERT_TRUE(store.snapshot());
        ASSERT_TRUE(std::filesystem::exists(tf.snap));
    }
}

// =============================================================================
// LC12. Store with active cleanup thread: run_cleanup_now() is safe to call
//       from the main thread while the background thread is also running.
// =============================================================================
TEST(lc12_run_cleanup_now_safe_with_background_thread) {
    TempFiles tf("lc12");
    auto store = make_store(tf.wal);
    store.set_with_ttl("exp", "v", 3600.0); // won't expire during test

    // Calling run_cleanup_now() while background thread is active must not
    // deadlock or crash.
    for (int i = 0; i < 10; ++i) {
        store.run_cleanup_now();
    }
    ASSERT_TRUE(store.exists("exp"));
}

// =============================================================================
// LC13. Move-constructed store inherits state and cleanup thread works.
// =============================================================================
TEST(lc13_move_constructed_store) {
    TempFiles tf("lc13");
    auto store1 = make_store(tf.wal);
    store1.set("move_key", "move_val");

    // Move-construct.
    forgekv::KeyValueStore store2(std::move(store1));

    ASSERT_HAS_VALUE(store2.get("move_key"));
    ASSERT_EQ(*store2.get("move_key"), "move_val");

    // Verify the moved-into store can still accept new writes.
    store2.set("extra", "after_move");
    ASSERT_EQ(*store2.get("extra"), "after_move");
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Lifecycle and Resource Tests\n";
    std::cout << std::string(50, '=') << "\n\n";
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
    std::cout << "\n" << std::string(50, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
