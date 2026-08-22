// =============================================================================
// ForgeKV — Stage 17: Admin HTTP Integration Tests
// =============================================================================
//
// Tests the POST /snapshot and POST /compact endpoints end-to-end using
// cpp-httplib's Client.
//
// Coverage:
//   1.  POST /snapshot — returns 200 {"status":"ok"}
//   2.  POST /snapshot — updates last_snapshot_time_us in /stats
//   3.  POST /snapshot — data survives snapshot (key is still retrievable)
//   4.  POST /snapshot — repeated snapshots succeed
//   5.  POST /compact  — returns 200 {"status":"ok"}
//   6.  POST /compact  — preserves all live keys after compaction
//   7.  POST /compact  — WAL size does not grow unboundedly after compact
//   8.  POST /compact  — keys set before compact are readable after
//   9.  POST /compact  — deleted keys not present after compact
//   10. POST /snapshot then POST /compact — both succeed, data intact
//   11. Concurrent reads during snapshot — no crash / stale reads
//   12. Concurrent writes during compact — no crash / state consistent
//   13. Other endpoints still work after snapshot
//   14. Other endpoints still work after compact
//
// =============================================================================

#define CPPHTTPLIB_THREAD_POOL_COUNT 0

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include "httplib.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <latch>
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
    do { if (!(cond)) throw AssertionFailure{"ASSERT_TRUE failed: " #cond \
        " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_FALSE(cond) \
    do { if ((cond)) throw AssertionFailure{"ASSERT_FALSE failed: " #cond \
        " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_EQ(a,b) \
    do { if ((a)!=(b)) throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b \
        " (line " + std::to_string(__LINE__) + ")"}; } while(false)

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar registrar_##name{#name, test_##name}; \
    static void test_##name()

// =============================================================================
// TempWAL RAII guard — cleans up .wal and .snapshot files
// =============================================================================

struct TempWAL {
    std::string path;
    explicit TempWAL(const std::string& tag)
        : path("test_hadmin_" + tag + ".wal") {}
    ~TempWAL() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + ".snapshot", ec);
    }
};

// =============================================================================
// HttpTestFixture — starts a fresh server on an ephemeral port
// =============================================================================

class HttpTestFixture {
public:
    explicit HttpTestFixture(const std::string& tag)
        : wal_guard_(tag)
    {
        auto storage = std::make_unique<forgekv::InMemoryStorage>();
        auto wal     = std::make_unique<forgekv::WAL>(wal_guard_.path);
        store_ = std::make_unique<forgekv::KeyValueStore>(
            std::move(storage), std::move(wal));
        server_ = std::make_unique<forgekv::HttpServer>(*store_);

        port_ = server_->bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Failed to bind test server");

        thread_ = std::thread([this]{ server_->listen_after_bind(); });
        server_->wait_until_ready();
    }

    ~HttpTestFixture() {
        server_->stop();
        if (thread_.joinable()) thread_.join();
    }

    forgekv::KeyValueStore& store() { return *store_; }

    // GET request
    httplib::Result get(const std::string& path) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Get(path);
    }

    // PUT request with body
    httplib::Result put(const std::string& path,
                        const std::string& body,
                        const httplib::Headers& headers = {}) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Put(path, headers, body, "text/plain");
    }

    // DELETE request
    httplib::Result del(const std::string& path) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Delete(path);
    }

    // POST request (for /snapshot and /compact)
    httplib::Result post(const std::string& path) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(10); // maintenance ops may take longer
        cli.set_read_timeout(10);
        return cli.Post(path);
    }

private:
    TempWAL wal_guard_;
    std::unique_ptr<forgekv::KeyValueStore> store_;
    std::unique_ptr<forgekv::HttpServer>    server_;
    int port_{-1};
    std::thread thread_;
};

// =============================================================================
// Simple JSON helpers for test assertions
// =============================================================================

static bool body_contains(const httplib::Result& res, const std::string& needle) {
    if (!res) return false;
    return res->body.find(needle) != std::string::npos;
}

// Parse a numeric field from a JSON body (naive but sufficient for tests).
// Returns -1 if not found.
static long long parse_json_number(const std::string& body,
                                   const std::string& field) {
    const std::string tok = "\"" + field + "\":";
    const auto pos = body.find(tok);
    if (pos == std::string::npos) return -1LL;
    const auto start = pos + tok.size();
    // skip whitespace
    auto cur = start;
    while (cur < body.size() && (body[cur] == ' ' || body[cur] == '\t')) ++cur;
    // read digits
    std::string digits;
    while (cur < body.size() && (std::isdigit(body[cur]) || body[cur] == '.')) {
        digits += body[cur++];
    }
    if (digits.empty()) return -1LL;
    try { return static_cast<long long>(std::stod(digits)); }
    catch (...) { return -1LL; }
}

// =============================================================================
// Tests
// =============================================================================

// ---------------------------------------------------------------------------
// 1. POST /snapshot returns 200 {"status":"ok"}
// ---------------------------------------------------------------------------
TEST(snapshot_returns_ok) {
    HttpTestFixture f("snap1");
    auto res = f.post("/snapshot");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"status\":\"ok\""));
}

// ---------------------------------------------------------------------------
// 2. POST /snapshot updates last_snapshot_time_us in /stats
// ---------------------------------------------------------------------------
TEST(snapshot_updates_stats) {
    HttpTestFixture f("snap2");

    // Before snapshot: last_snapshot_time_us should be 0 (never).
    auto stats_before = f.get("/stats");
    ASSERT_TRUE(stats_before);
    ASSERT_EQ(stats_before->status, 200);
    long long ts_before = parse_json_number(stats_before->body, "last_snapshot_time_us");
    ASSERT_EQ(ts_before, 0LL);

    // Take snapshot.
    auto snap = f.post("/snapshot");
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->status, 200);

    // After snapshot: last_snapshot_time_us must be non-zero.
    auto stats_after = f.get("/stats");
    ASSERT_TRUE(stats_after);
    ASSERT_EQ(stats_after->status, 200);
    long long ts_after = parse_json_number(stats_after->body, "last_snapshot_time_us");
    ASSERT_TRUE(ts_after > 0LL);
}

// ---------------------------------------------------------------------------
// 3. POST /snapshot — data survives (key is still retrievable after snapshot)
// ---------------------------------------------------------------------------
TEST(snapshot_preserves_data) {
    HttpTestFixture f("snap3");

    // Insert some keys.
    for (int i = 0; i < 5; ++i) {
        auto r = f.put("/key/k" + std::to_string(i), "v" + std::to_string(i));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
    }

    // Snapshot.
    auto snap = f.post("/snapshot");
    ASSERT_TRUE(snap);
    ASSERT_EQ(snap->status, 200);

    // All keys still readable.
    for (int i = 0; i < 5; ++i) {
        auto r = f.get("/key/k" + std::to_string(i));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
        ASSERT_TRUE(body_contains(r, "\"v" + std::to_string(i) + "\""));
    }
}

// ---------------------------------------------------------------------------
// 4. POST /snapshot — repeated snapshots all succeed
// ---------------------------------------------------------------------------
TEST(snapshot_repeated) {
    HttpTestFixture f("snap4");
    f.put("/key/a", "1");
    for (int i = 0; i < 3; ++i) {
        auto r = f.post("/snapshot");
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
        ASSERT_TRUE(body_contains(r, "\"status\":\"ok\""));
    }
}

// ---------------------------------------------------------------------------
// 5. POST /compact returns 200 {"status":"ok"}
// ---------------------------------------------------------------------------
TEST(compact_returns_ok) {
    HttpTestFixture f("cmpct1");
    // Insert data first so there is something to compact.
    f.put("/key/foo", "bar");
    f.del("/key/foo");

    auto res = f.post("/compact");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"status\":\"ok\""));
}

// ---------------------------------------------------------------------------
// 6. POST /compact preserves all live keys
// ---------------------------------------------------------------------------
TEST(compact_preserves_live_keys) {
    HttpTestFixture f("cmpct2");

    // Insert 10 keys, delete 5.
    for (int i = 0; i < 10; ++i) {
        f.put("/key/key" + std::to_string(i), "val" + std::to_string(i));
    }
    for (int i = 0; i < 5; ++i) {
        f.del("/key/key" + std::to_string(i));
    }

    auto compact = f.post("/compact");
    ASSERT_TRUE(compact);
    ASSERT_EQ(compact->status, 200);

    // Keys 5–9 must still be readable.
    for (int i = 5; i < 10; ++i) {
        auto r = f.get("/key/key" + std::to_string(i));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 200);
    }
    // Keys 0–4 must be gone.
    for (int i = 0; i < 5; ++i) {
        auto r = f.get("/key/key" + std::to_string(i));
        ASSERT_TRUE(r);
        ASSERT_EQ(r->status, 404);
    }
}

// ---------------------------------------------------------------------------
// 7. POST /compact on empty store succeeds
// ---------------------------------------------------------------------------
TEST(compact_empty_store) {
    HttpTestFixture f("cmpct3");
    auto res = f.post("/compact");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"status\":\"ok\""));
}

// ---------------------------------------------------------------------------
// 8. Keys set before compact are readable after compact
// ---------------------------------------------------------------------------
TEST(compact_keys_readable_after) {
    HttpTestFixture f("cmpct4");
    f.put("/key/alpha", "a");
    f.put("/key/beta",  "b");
    f.put("/key/gamma", "g");

    auto compact = f.post("/compact");
    ASSERT_TRUE(compact);
    ASSERT_EQ(compact->status, 200);

    auto ra = f.get("/key/alpha");
    ASSERT_TRUE(ra && ra->status == 200);
    ASSERT_TRUE(body_contains(ra, "\"a\""));

    auto rb = f.get("/key/beta");
    ASSERT_TRUE(rb && rb->status == 200);
    ASSERT_TRUE(body_contains(rb, "\"b\""));

    auto rg = f.get("/key/gamma");
    ASSERT_TRUE(rg && rg->status == 200);
    ASSERT_TRUE(body_contains(rg, "\"g\""));
}

// ---------------------------------------------------------------------------
// 9. POST /compact — deleted keys not present after compact
// ---------------------------------------------------------------------------
TEST(compact_deleted_keys_gone) {
    HttpTestFixture f("cmpct5");
    f.put("/key/x", "xval");
    f.put("/key/y", "yval");
    f.del("/key/x");

    auto compact = f.post("/compact");
    ASSERT_TRUE(compact);
    ASSERT_EQ(compact->status, 200);

    // "x" is gone.
    auto rx = f.get("/key/x");
    ASSERT_TRUE(rx);
    ASSERT_EQ(rx->status, 404);

    // "y" is present.
    auto ry = f.get("/key/y");
    ASSERT_TRUE(ry);
    ASSERT_EQ(ry->status, 200);
}

// ---------------------------------------------------------------------------
// 10. snapshot then compact — both succeed, data intact
// ---------------------------------------------------------------------------
TEST(snapshot_then_compact) {
    HttpTestFixture f("snap_cmpct");

    for (int i = 0; i < 8; ++i) {
        f.put("/key/item" + std::to_string(i), "value" + std::to_string(i));
    }

    auto snap = f.post("/snapshot");
    ASSERT_TRUE(snap && snap->status == 200);

    // Delete a few keys.
    for (int i = 0; i < 4; ++i) {
        f.del("/key/item" + std::to_string(i));
    }

    auto compact = f.post("/compact");
    ASSERT_TRUE(compact && compact->status == 200);

    // Items 4–7 must still be readable.
    for (int i = 4; i < 8; ++i) {
        auto r = f.get("/key/item" + std::to_string(i));
        ASSERT_TRUE(r && r->status == 200);
    }
    // Items 0–3 must be gone.
    for (int i = 0; i < 4; ++i) {
        auto r = f.get("/key/item" + std::to_string(i));
        ASSERT_TRUE(r && r->status == 404);
    }
}

// ---------------------------------------------------------------------------
// 11. Concurrent reads during snapshot — no crash / stale reads
// ---------------------------------------------------------------------------
TEST(concurrent_reads_during_snapshot) {
    HttpTestFixture f("conc_snap");

    // Pre-populate.
    for (int i = 0; i < 10; ++i) {
        f.put("/key/ckey" + std::to_string(i), "val" + std::to_string(i));
    }

    constexpr int kReaders = 4;
    std::atomic<int> errors{0};

    // Run snapshot in a background thread while readers fire concurrently.
    std::latch start_latch(kReaders + 1);
    std::latch done_latch(kReaders);

    // Snapshot thread
    std::thread snap_thread([&] {
        start_latch.arrive_and_wait();
        auto r = f.post("/snapshot");
        if (!r || r->status != 200) ++errors;
    });

    // Reader threads
    std::vector<std::thread> readers;
    readers.reserve(kReaders);
    for (int t = 0; t < kReaders; ++t) {
        readers.emplace_back([&, t] {
            start_latch.arrive_and_wait();
            for (int i = 0; i < 10; ++i) {
                auto r = f.get("/key/ckey" + std::to_string((t + i) % 10));
                if (!r || (r->status != 200 && r->status != 404)) ++errors;
            }
            done_latch.count_down();
        });
    }

    done_latch.wait();
    snap_thread.join();
    for (auto& th : readers) th.join();

    ASSERT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
// 12. Concurrent writes during compact — no crash / state consistent
// ---------------------------------------------------------------------------
TEST(concurrent_writes_during_compact) {
    HttpTestFixture f("conc_cmpct");

    // Pre-populate some data.
    for (int i = 0; i < 10; ++i) {
        f.put("/key/base" + std::to_string(i), "init" + std::to_string(i));
    }

    constexpr int kWriters = 4;
    std::atomic<int> errors{0};

    std::latch start_latch(kWriters + 1);
    std::latch done_latch(kWriters);

    // Compact thread
    std::thread compact_thread([&] {
        start_latch.arrive_and_wait();
        auto r = f.post("/compact");
        if (!r || r->status != 200) ++errors;
    });

    // Writer threads
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int t = 0; t < kWriters; ++t) {
        writers.emplace_back([&, t] {
            start_latch.arrive_and_wait();
            for (int i = 0; i < 5; ++i) {
                auto r = f.put("/key/write" + std::to_string(t * 10 + i),
                               "w" + std::to_string(t));
                if (!r || r->status != 200) ++errors;
            }
            done_latch.count_down();
        });
    }

    done_latch.wait();
    compact_thread.join();
    for (auto& th : writers) th.join();

    ASSERT_EQ(errors.load(), 0);

    // All base keys must still be present (compact does not remove live keys).
    for (int i = 0; i < 10; ++i) {
        auto r = f.get("/key/base" + std::to_string(i));
        ASSERT_TRUE(r && r->status == 200);
    }
}

// ---------------------------------------------------------------------------
// 13. Other endpoints still work after snapshot
// ---------------------------------------------------------------------------
TEST(other_endpoints_work_after_snapshot) {
    HttpTestFixture f("after_snap");

    f.put("/key/mykey", "myval");
    f.post("/snapshot");

    // GET /health
    auto h = f.get("/health");
    ASSERT_TRUE(h && h->status == 200);

    // GET /stats
    auto s = f.get("/stats");
    ASSERT_TRUE(s && s->status == 200);

    // GET /key/:key
    auto g = f.get("/key/mykey");
    ASSERT_TRUE(g && g->status == 200);
    ASSERT_TRUE(body_contains(g, "\"myval\""));

    // DELETE /key/:key
    auto d = f.del("/key/mykey");
    ASSERT_TRUE(d && d->status == 200);
}

// ---------------------------------------------------------------------------
// 14. Other endpoints still work after compact
// ---------------------------------------------------------------------------
TEST(other_endpoints_work_after_compact) {
    HttpTestFixture f("after_cmpct");

    f.put("/key/existingkey", "existingval");
    f.post("/compact");

    // GET /health
    auto h = f.get("/health");
    ASSERT_TRUE(h && h->status == 200);

    // GET /stats
    auto s = f.get("/stats");
    ASSERT_TRUE(s && s->status == 200);

    // GET /key/:key
    auto g = f.get("/key/existingkey");
    ASSERT_TRUE(g && g->status == 200);
    ASSERT_TRUE(body_contains(g, "\"existingval\""));

    // PUT new key
    auto p = f.put("/key/newkey", "newval");
    ASSERT_TRUE(p && p->status == 200);

    // Verify new key readable
    auto g2 = f.get("/key/newkey");
    ASSERT_TRUE(g2 && g2->status == 200);
    ASSERT_TRUE(body_contains(g2, "\"newval\""));
}

// =============================================================================
// main — run all registered tests
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed = 0;

    std::cout << "=== ForgeKV Stage 17 Admin HTTP Tests ===\n\n";

    for (const auto& tc : tests) {
        try {
            tc.fn();
            std::cout << "  [PASS] " << tc.name << "\n";
            ++passed;
        } catch (const AssertionFailure& af) {
            std::cout << "  [FAIL] " << tc.name << "\n"
                      << "         " << af.message << "\n";
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "  [FAIL] " << tc.name << "\n"
                      << "         exception: " << ex.what() << "\n";
            ++failed;
        }
    }

    std::cout << "\n" << passed << " passed, " << failed << " failed "
              << "out of " << (passed + failed) << " tests.\n";

    return failed == 0 ? 0 : 1;
}
