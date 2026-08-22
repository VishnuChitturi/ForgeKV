// =============================================================================
// ForgeKV — Stage 13: Concurrency Hardening Tests
// =============================================================================
//
// Tests the shared_mutex-based synchronization under concurrent workloads.
// Uses std::barrier, std::latch, and std::atomic — no arbitrary sleeps for
// synchronization. Tests detect crashes, data races (via Address/Thread
// Sanitizer), deadlocks, and inconsistent state.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <latch>
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
        : wal("test_ch_" + name + ".wal")
        , snap("test_ch_" + name + ".wal.snapshot") {}
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
// CH1. Concurrent GET: all threads see consistent values (no crash, no
//      corrupted strings). No writes during this test.
// =============================================================================
TEST(ch1_concurrent_get_no_crash) {
    TempFiles tf("ch1");
    auto store = make_store(tf.wal);

    const int KEY_COUNT = 20;
    const int THREADS   = 8;
    const int ROUNDS    = 100;

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("k" + std::to_string(i), "v" + std::to_string(i));
    }

    std::latch ready(THREADS);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int r = 0; r < ROUNDS; ++r) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto v = store.get("k" + std::to_string(i));
                    if (!v.has_value() || *v != "v" + std::to_string(i)) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!store.exists("k" + std::to_string(i))) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
}

// =============================================================================
// CH2. Concurrent SET to disjoint keys: all writes visible after join.
// =============================================================================
TEST(ch2_concurrent_set_disjoint_keys) {
    TempFiles tf("ch2");
    auto store = make_store(tf.wal);

    const int THREADS   = 6;
    const int KEYS_EACH = 30;

    std::latch ready(THREADS);
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int i = 0; i < KEYS_EACH; ++i) {
                store.set("t" + std::to_string(t) + "_k" + std::to_string(i),
                          "v" + std::to_string(t * KEYS_EACH + i));
            }
        });
    }

    for (auto& th : threads) th.join();

    ASSERT_EQ(store.size(), static_cast<std::size_t>(THREADS * KEYS_EACH));
    for (int t = 0; t < THREADS; ++t) {
        for (int i = 0; i < KEYS_EACH; ++i) {
            auto v = store.get("t" + std::to_string(t) + "_k" + std::to_string(i));
            ASSERT_TRUE(v.has_value());
        }
    }
}

// =============================================================================
// CH3. Concurrent GET + SET: readers never see corrupted/empty values.
// =============================================================================
TEST(ch3_concurrent_get_and_set) {
    TempFiles tf("ch3");
    auto store = make_store(tf.wal);

    const int KEY_COUNT    = 10;
    const int WRITER_T     = 4;
    const int READER_T     = 4;
    const int WRITE_OPS    = 200;

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("mk" + std::to_string(i), "init");
    }

    std::latch ready(WRITER_T + READER_T);
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < WRITER_T; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < WRITE_OPS; ++op) {
                store.set("mk" + std::to_string(op % KEY_COUNT),
                          "w" + std::to_string(t) + "_" + std::to_string(op));
            }
        });
    }

    for (int t = 0; t < READER_T; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            while (!done.load(std::memory_order_acquire)) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto v = store.get("mk" + std::to_string(i));
                    if (v.has_value() && v->empty()) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (int t = 0; t < WRITER_T; ++t) threads[t].join();
    done.store(true, std::memory_order_release);
    for (int t = WRITER_T; t < WRITER_T + READER_T; ++t) threads[t].join();

    ASSERT_EQ(errors.load(), 0);
}

// =============================================================================
// CH4. Concurrent GET + DELETE: no crash, consistent state after join.
// =============================================================================
TEST(ch4_concurrent_get_and_delete) {
    TempFiles tf("ch4");
    auto store = make_store(tf.wal);

    const int KEY_COUNT = 10;
    const int DEL_T     = 4;
    const int READ_T    = 4;
    const int OPS       = 100;

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("dk" + std::to_string(i), "val");
    }

    std::latch ready(DEL_T + READ_T);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < DEL_T; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                store.del("dk" + std::to_string((op + t) % KEY_COUNT));
            }
        });
    }

    for (int t = 0; t < READ_T; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto v = store.get("dk" + std::to_string(i));
                    // Value, if present, must be non-empty.
                    if (v.has_value() && v->empty()) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
    // size() must be in [0, KEY_COUNT].
    ASSERT_TRUE(store.size() <= static_cast<std::size_t>(KEY_COUNT));
}

// =============================================================================
// CH5. Concurrent SET + DELETE on same key: no crash, valid final state.
// =============================================================================
TEST(ch5_concurrent_set_delete_same_key) {
    TempFiles tf("ch5");
    auto store = make_store(tf.wal);

    const int SET_T = 4;
    const int DEL_T = 4;
    const int OPS   = 150;

    std::latch ready(SET_T + DEL_T);
    std::vector<std::thread> threads;

    for (int t = 0; t < SET_T; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                store.set("race", "val_t" + std::to_string(t));
            }
        });
    }
    for (int t = 0; t < DEL_T; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                store.del("race");
            }
        });
    }

    for (auto& th : threads) th.join();

    // Final state: key either absent or has a valid non-empty value.
    auto v = store.get("race");
    if (v.has_value()) {
        ASSERT_FALSE(v->empty());
    }
    // size() must be 0 or 1.
    ASSERT_TRUE(store.size() <= std::size_t{1});
}

// =============================================================================
// CH6. Concurrent mixed operations (SET/GET/DELETE/exists/size): no crash.
// =============================================================================
TEST(ch6_concurrent_mixed_ops_no_crash) {
    TempFiles tf("ch6");
    auto store = make_store(tf.wal);

    const int THREADS = 8;
    const int OPS     = 200;

    for (int i = 0; i < 10; ++i) {
        store.set("base" + std::to_string(i), "v");
    }

    std::latch ready(THREADS);
    std::atomic<int> exceptions{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            try {
                for (int op = 0; op < OPS; ++op) {
                    const std::string key = "base" + std::to_string(op % 10);
                    switch ((t * OPS + op) % 5) {
                        case 0: store.set(key, "upd" + std::to_string(op)); break;
                        case 1: (void)store.get(key); break;
                        case 2: store.del(key); break;
                        case 3: (void)store.exists(key); break;
                        case 4: (void)store.size(); break;
                    }
                }
            } catch (...) {
                exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(exceptions.load(), 0);
}

// =============================================================================
// CH7. Concurrent TTL operations: set_with_ttl + ttl() + run_cleanup_now.
// =============================================================================
TEST(ch7_concurrent_ttl_operations) {
    TempFiles tf("ch7");
    auto store = make_store(tf.wal);

    const int THREADS = 6;
    const int OPS     = 100;

    std::latch ready(THREADS);
    std::atomic<int> exceptions{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            try {
                for (int op = 0; op < OPS; ++op) {
                    const std::string key = "ttl_k" + std::to_string((t * OPS + op) % 20);
                    if (op % 3 == 0) {
                        store.set_with_ttl(key, "v", 3600.0);
                    } else if (op % 3 == 1) {
                        (void)store.ttl(key);
                    } else {
                        (void)store.exists(key);
                    }
                }
            } catch (...) {
                exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(exceptions.load(), 0);
}

// =============================================================================
// CH8. Concurrent stats() calls: no crash, values are non-negative.
// =============================================================================
TEST(ch8_concurrent_stats_no_crash) {
    TempFiles tf("ch8");
    auto store = make_store(tf.wal);
    store.set("seed", "v");

    const int THREADS = 8;
    const int OPS     = 200;

    std::latch ready(THREADS);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                if (t % 2 == 0) {
                    store.set("k" + std::to_string(op), "v");
                } else {
                    auto s = store.stats();
                    if (s.uptime_seconds < 0.0) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
}

// =============================================================================
// CH9. Concurrent snapshot() with active writers: no deadlock, no crash.
//      After all threads finish, restart produces a consistent state.
// =============================================================================
TEST(ch9_concurrent_snapshot_with_writers) {
    TempFiles tf("ch9");
    auto store = make_store(tf.wal);

    const int WRITER_T  = 4;
    const int WRITE_OPS = 100;
    const int KEY_COUNT = 15;

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("sk" + std::to_string(i), "init");
    }

    std::latch ready(WRITER_T + 1);
    std::atomic<int> exceptions{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < WRITER_T; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < WRITE_OPS; ++op) {
                store.set("sk" + std::to_string(op % KEY_COUNT),
                          "t" + std::to_string(t) + "_" + std::to_string(op));
            }
        });
    }

    // Snapshot thread.
    bool snap_ok = false;
    std::thread snap_thread([&]() {
        ready.arrive_and_wait();
        snap_ok = store.snapshot();
    });

    for (auto& th : threads) th.join();
    snap_thread.join();

    ASSERT_TRUE(snap_ok);
    ASSERT_EQ(exceptions.load(), 0);

    const std::size_t live_count = store.size();

    // Restart must produce a consistent state (no exception, correct count).
    auto s2 = make_store(tf.wal);
    ASSERT_EQ(s2.size(), live_count);
}

// =============================================================================
// CH10. Concurrent compact() with readers: no crash, state correct after.
// =============================================================================
TEST(ch10_concurrent_compact_with_readers) {
    TempFiles tf("ch10");
    auto store = make_store(tf.wal);

    const int KEY_COUNT   = 20;
    const int READER_T    = 4;
    const int READ_ROUNDS = 50;

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("ck" + std::to_string(i), "v" + std::to_string(i));
    }

    std::latch ready(READER_T + 1);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < READER_T; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int r = 0; r < READ_ROUNDS; ++r) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto v = store.get("ck" + std::to_string(i));
                    if (!v.has_value()) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    std::thread compact_thread([&]() {
        ready.arrive_and_wait();
        store.compact();
    });

    for (auto& th : threads) th.join();
    compact_thread.join();

    ASSERT_EQ(errors.load(), 0);
    ASSERT_EQ(store.size(), static_cast<std::size_t>(KEY_COUNT));
}

// =============================================================================
// CH11. std::barrier: phased concurrent writes then verify all keys present.
// =============================================================================
TEST(ch11_barrier_phased_writes) {
    TempFiles tf("ch11");
    auto store = make_store(tf.wal);

    const int THREADS   = 4;
    const int KEYS_EACH = 25;

    // Phase 1: all threads write their own keys.
    // Phase 2: all threads verify all keys.
    std::barrier<> sync(THREADS);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            // Phase 1: write.
            for (int i = 0; i < KEYS_EACH; ++i) {
                store.set("ph_t" + std::to_string(t) + "_k" + std::to_string(i),
                          "val");
            }
            sync.arrive_and_wait();
            // Phase 2: verify all threads' keys.
            for (int other = 0; other < THREADS; ++other) {
                for (int i = 0; i < KEYS_EACH; ++i) {
                    if (!store.exists("ph_t" + std::to_string(other) +
                                      "_k" + std::to_string(i))) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
    ASSERT_EQ(store.size(), static_cast<std::size_t>(THREADS * KEYS_EACH));
}

// =============================================================================
// CH12. Concurrent writes + TTL expiration + snapshot: internally consistent.
// =============================================================================
TEST(ch12_concurrent_writes_ttl_snapshot) {
    TempFiles tf("ch12");
    auto store = make_store(tf.wal);

    const int WRITER_T  = 4;
    const int WRITE_OPS = 80;

    for (int i = 0; i < 10; ++i) {
        store.set("base" + std::to_string(i), "v");
    }

    std::latch ready(WRITER_T + 1);
    std::atomic<int> exceptions{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < WRITER_T; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            try {
                for (int op = 0; op < WRITE_OPS; ++op) {
                    if (op % 4 == 0) {
                        store.set_with_ttl("ttl_k" + std::to_string(op % 5),
                                           "v", 3600.0);
                    } else {
                        store.set("base" + std::to_string((t * WRITE_OPS + op) % 10),
                                  "upd");
                    }
                }
            } catch (...) {
                exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    bool snap_ok = false;
    std::thread snap_thread([&]() {
        ready.arrive_and_wait();
        snap_ok = store.snapshot();
    });

    for (auto& th : threads) th.join();
    snap_thread.join();

    ASSERT_TRUE(snap_ok);
    ASSERT_EQ(exceptions.load(), 0);

    // Restart from snapshot + WAL tail: must not throw and must be consistent.
    const std::size_t live = store.size();
    auto s2 = make_store(tf.wal);
    ASSERT_EQ(s2.size(), live);
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Concurrency Hardening Tests\n";
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
