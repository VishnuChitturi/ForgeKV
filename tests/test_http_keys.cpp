// =============================================================================
// ForgeKV — Stage 16: GET /keys HTTP Integration Tests
// =============================================================================
//
// Tests the GET /keys endpoint end-to-end using cpp-httplib's Client.
//
// Coverage:
//   1.  Empty database → total=0, keys=[]
//   2.  Single key     → appears in response with correct metadata
//   3.  Multiple keys  → all appear, lexicographic order
//   4.  Prefix filter  → only matching keys returned
//   5.  Prefix no-match → empty result, total=0
//   6.  Limit          → at most limit keys returned
//   7.  Offset         → correct starting position
//   8.  Limit + offset → combined pagination
//   9.  Deterministic ordering → repeated requests same order
//   10. Last page      → offset past end → empty keys, total still correct
//   11. Invalid limit (negative)          → 400
//   12. Invalid limit (non-numeric)       → 400
//   13. Invalid offset (negative)         → 400
//   14. Invalid offset (non-numeric)      → 400
//   15. Excessively large limit clamped to 100
//   16. TTL metadata — permanent key      → ttl_seconds == -1
//   17. TTL metadata — expiring key       → ttl_seconds >= 0
//   18. Expired keys excluded
//   19. Concurrent listing + writes       → no crash / consistent data
//   20. Special characters in keys        → JSON-escaped correctly
//   21. existing endpoints still work after adding /keys
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
// Minimal test harness (same style as other HTTP test files)
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
// TempWAL RAII guard
// =============================================================================

struct TempWAL {
    std::string path;
    explicit TempWAL(const std::string& tag)
        : path("test_hkeys_" + tag + ".wal") {}
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

    httplib::Result get(const std::string& path) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Get(path);
    }

    httplib::Result put(const std::string& path,
                        const std::string& body,
                        const httplib::Headers& headers = {}) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Put(path, headers, body, "text/plain");
    }

    httplib::Result del(const std::string& path) {
        httplib::Client cli("127.0.0.1", port_);
        cli.set_connection_timeout(5);
        cli.set_read_timeout(5);
        return cli.Delete(path);
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

// Check that the response body contains a substring.
static bool body_contains(const httplib::Result& res, const std::string& needle) {
    if (!res) return false;
    return res->body.find(needle) != std::string::npos;
}

// Parse "total": N from a response body (naive, but sufficient for tests).
static long parse_total(const std::string& body) {
    const std::string tok = "\"total\":";
    auto pos = body.find(tok);
    if (pos == std::string::npos) return -1;
    pos += tok.size();
    return std::stol(body.substr(pos));
}

// Count how many times "\"key\":" appears in the body (= number of key entries).
static long count_key_entries(const std::string& body) {
    long count = 0;
    std::size_t pos = 0;
    const std::string needle = "\"key\":\"";
    while ((pos = body.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// =============================================================================
// Tests
// =============================================================================

// 1. Empty database
TEST(keys_empty_database) {
    HttpTestFixture f("empty");
    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"keys\":[]"));
    ASSERT_EQ(parse_total(res->body), 0);
}

// 2. Single key
TEST(keys_single_key) {
    HttpTestFixture f("single");
    f.store().set("hello", "world");

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 1);
    ASSERT_EQ(count_key_entries(res->body), 1);
    ASSERT_TRUE(body_contains(res, "\"key\":\"hello\""));
    ASSERT_TRUE(body_contains(res, "\"value\":\"world\""));
    ASSERT_TRUE(body_contains(res, "\"ttl_seconds\":-1"));
}

// 3. Multiple keys — all appear, lexicographic order
TEST(keys_multiple_keys_ordered) {
    HttpTestFixture f("multi");
    // Insert in reverse order; response must be lexicographic.
    f.store().set("zebra", "z");
    f.store().set("apple", "a");
    f.store().set("mango", "m");

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 3);
    ASSERT_EQ(count_key_entries(res->body), 3);

    // Verify lexicographic order by finding positions.
    const auto& body = res->body;
    auto pa = body.find("\"apple\"");
    auto pm = body.find("\"mango\"");
    auto pz = body.find("\"zebra\"");
    ASSERT_TRUE(pa != std::string::npos);
    ASSERT_TRUE(pm != std::string::npos);
    ASSERT_TRUE(pz != std::string::npos);
    ASSERT_TRUE(pa < pm);
    ASSERT_TRUE(pm < pz);
}

// 4. Prefix filter — matching keys only
TEST(keys_prefix_filter_match) {
    HttpTestFixture f("prefix_match");
    f.store().set("user:1", "Alice");
    f.store().set("user:2", "Bob");
    f.store().set("config:theme", "dark");

    auto res = f.get("/keys?prefix=user:");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 2);
    ASSERT_EQ(count_key_entries(res->body), 2);
    ASSERT_TRUE(body_contains(res, "user:1"));
    ASSERT_TRUE(body_contains(res, "user:2"));
    ASSERT_FALSE(body_contains(res, "config:theme"));
}

// 5. Prefix filter — no match
TEST(keys_prefix_filter_no_match) {
    HttpTestFixture f("prefix_nomatch");
    f.store().set("foo", "bar");

    auto res = f.get("/keys?prefix=xyz");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 0);
    ASSERT_TRUE(body_contains(res, "\"keys\":[]"));
}

// 6. Limit
TEST(keys_limit) {
    HttpTestFixture f("limit");
    for (int i = 0; i < 10; ++i) {
        f.store().set("key:" + std::to_string(i), "v");
    }
    auto res = f.get("/keys?limit=3");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(count_key_entries(res->body), 3);
    ASSERT_EQ(parse_total(res->body), 10); // total unchanged
    ASSERT_TRUE(body_contains(res, "\"limit\":3"));
}

// 7. Offset
TEST(keys_offset) {
    HttpTestFixture f("offset");
    // Insert a, b, c, d, e — lexicographic
    for (char c = 'a'; c <= 'e'; ++c) {
        f.store().set(std::string(1, c), "v");
    }
    // offset=2 should skip 'a', 'b' and start at 'c'
    auto res = f.get("/keys?offset=2");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(count_key_entries(res->body), 3); // c, d, e
    ASSERT_FALSE(body_contains(res, "\"key\":\"a\""));
    ASSERT_FALSE(body_contains(res, "\"key\":\"b\""));
    ASSERT_TRUE(body_contains(res, "\"key\":\"c\""));
}

// 8. Limit + offset combined
TEST(keys_limit_and_offset) {
    HttpTestFixture f("limit_offset");
    for (char c = 'a'; c <= 'f'; ++c) {
        f.store().set(std::string(1, c), "v");
    }
    // offset=2, limit=2 → c, d
    auto res = f.get("/keys?limit=2&offset=2");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(count_key_entries(res->body), 2);
    ASSERT_TRUE(body_contains(res, "\"key\":\"c\""));
    ASSERT_TRUE(body_contains(res, "\"key\":\"d\""));
    ASSERT_FALSE(body_contains(res, "\"key\":\"a\""));
    ASSERT_FALSE(body_contains(res, "\"key\":\"e\""));
}

// 9. Deterministic ordering — same request twice, same order
TEST(keys_deterministic_order) {
    HttpTestFixture f("determ");
    f.store().set("z", "1");
    f.store().set("a", "2");
    f.store().set("m", "3");

    auto res1 = f.get("/keys");
    auto res2 = f.get("/keys");
    ASSERT_TRUE(res1);
    ASSERT_TRUE(res2);
    ASSERT_EQ(res1->body, res2->body);
}

// 10. Offset past end → empty keys[], total still reflects all
TEST(keys_offset_past_end) {
    HttpTestFixture f("past_end");
    f.store().set("only", "one");

    auto res = f.get("/keys?offset=100");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"keys\":[]"));
    ASSERT_EQ(parse_total(res->body), 1);
}

// 11. Invalid limit — negative
TEST(keys_invalid_limit_negative) {
    HttpTestFixture f("inv_limit_neg");
    auto res = f.get("/keys?limit=-5");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
    ASSERT_TRUE(body_contains(res, "error"));
}

// 12. Invalid limit — non-numeric
TEST(keys_invalid_limit_alpha) {
    HttpTestFixture f("inv_limit_alpha");
    auto res = f.get("/keys?limit=abc");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
    ASSERT_TRUE(body_contains(res, "error"));
}

// 13. Invalid offset — negative
TEST(keys_invalid_offset_negative) {
    HttpTestFixture f("inv_offset_neg");
    auto res = f.get("/keys?offset=-1");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
    ASSERT_TRUE(body_contains(res, "error"));
}

// 14. Invalid offset — non-numeric
TEST(keys_invalid_offset_alpha) {
    HttpTestFixture f("inv_offset_alpha");
    auto res = f.get("/keys?offset=bad");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
    ASSERT_TRUE(body_contains(res, "error"));
}

// 15. Excessively large limit is clamped to 100
TEST(keys_large_limit_clamped) {
    HttpTestFixture f("large_limit");
    // Add 5 keys; ask for limit=9999 — should get all 5 (clamped to 100 max).
    for (int i = 0; i < 5; ++i) {
        f.store().set("k" + std::to_string(i), "v");
    }
    auto res = f.get("/keys?limit=9999");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    // The effective limit in response should be 100 (clamped).
    ASSERT_TRUE(body_contains(res, "\"limit\":100"));
    // Still returns all 5 keys.
    ASSERT_EQ(count_key_entries(res->body), 5);
}

// 16. TTL metadata — permanent key → ttl_seconds == -1
TEST(keys_ttl_permanent) {
    HttpTestFixture f("ttl_perm");
    f.store().set("pkey", "pval");

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(body_contains(res, "\"ttl_seconds\":-1"));
}

// 17. TTL metadata — expiring key → ttl_seconds >= 0
TEST(keys_ttl_expiring) {
    HttpTestFixture f("ttl_expiring");
    f.store().set_with_ttl("ekey", "eval", 3600.0); // 1 hour

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    // ttl_seconds should be a large positive number (close to 3600).
    // Just verify it's not -1 and the key is present.
    ASSERT_TRUE(body_contains(res, "\"key\":\"ekey\""));
    // The response should NOT contain -1 for this key's ttl_seconds.
    // (Permanent marker -1 should only be for permanent keys.)
    // We verify by checking the response has no "-1.000" for this key's TTL.
    // Simplest: just ensure ttl_seconds appears and key is present.
    ASSERT_FALSE(body_contains(res, "\"ttl_seconds\":-1"));
}

// 18. Expired keys are excluded
TEST(keys_expired_excluded) {
    HttpTestFixture f("expired");
    // Set a key with a 1-microsecond TTL — effectively immediately expired.
    f.store().set_with_ttl("expiring", "gone", 0.000001);

    // Wait a moment to ensure it has logically expired.
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    // Force cleanup so the key is evicted.
    f.store().run_cleanup_now();

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 0);
    ASSERT_FALSE(body_contains(res, "expiring"));
}

// 19. Concurrent listing + writes — no crash or data corruption
TEST(keys_concurrent_listing_and_writes) {
    HttpTestFixture f("concurrent");

    // Seed a few keys.
    for (int i = 0; i < 10; ++i) {
        f.store().set("base:" + std::to_string(i), "v");
    }

    std::atomic<bool> stop{false};
    std::atomic<int>  errors{0};

    // Writer thread: continually PUT and DELETE keys.
    std::thread writer([&]() {
        int n = 0;
        while (!stop.load()) {
            f.store().set("live:" + std::to_string(n % 5), "x");
            f.store().del("live:" + std::to_string((n + 2) % 5));
            ++n;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Reader: issue 20 GET /keys requests.
    for (int i = 0; i < 20; ++i) {
        auto res = f.get("/keys");
        if (!res || res->status != 200) {
            ++errors;
        }
    }

    stop.store(true);
    writer.join();

    ASSERT_EQ(errors.load(), 0);
}

// 20. Special characters in keys — JSON-escaped correctly
TEST(keys_special_characters) {
    HttpTestFixture f("special");
    f.store().set("key with spaces", "val");
    f.store().set("key/with/slashes", "val");
    const std::string quoted_key   = "key\"quoted\"";
    const std::string backslash_key = "key\\backslash";
    f.store().set(quoted_key,    "val");
    f.store().set(backslash_key, "val");

    auto res = f.get("/keys");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(parse_total(res->body), 4);
    // Verify escaped forms appear in the body.
    ASSERT_TRUE(body_contains(res, "key with spaces"));
    ASSERT_TRUE(body_contains(res, "key/with/slashes"));
    // JSON-escaped double-quote: key\"quoted\"
    const std::string expected_quote = "key\\\"quoted\\\"";
    ASSERT_TRUE(body_contains(res, expected_quote));
    // JSON-escaped backslash: key\\backslash
    const std::string expected_bs = "key\\\\backslash";
    ASSERT_TRUE(body_contains(res, expected_bs));
}

// 21. Existing endpoints still work after adding /keys
TEST(existing_endpoints_unaffected) {
    HttpTestFixture f("compat");
    f.store().set("x", "y");

    // GET /key/:key
    auto g = f.get("/key/x");
    ASSERT_TRUE(g);
    ASSERT_EQ(g->status, 200);
    ASSERT_TRUE(body_contains(g, "\"value\":\"y\""));

    // GET /health
    auto h = f.get("/health");
    ASSERT_TRUE(h);
    ASSERT_EQ(h->status, 200);
    ASSERT_TRUE(body_contains(h, "\"status\":\"ok\""));

    // DELETE /key/:key
    auto d = f.del("/key/x");
    ASSERT_TRUE(d);
    ASSERT_EQ(d->status, 200);

    // After delete, /keys should be empty.
    auto k = f.get("/keys");
    ASSERT_TRUE(k);
    ASSERT_EQ(k->status, 200);
    ASSERT_EQ(parse_total(k->body), 0);
}

// =============================================================================
// main — run all registered tests
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;

    for (const auto& t : tests) {
        try {
            t.fn();
            std::cout << "[PASS] " << t.name << '\n';
            ++passed;
        } catch (const AssertionFailure& af) {
            std::cout << "[FAIL] " << t.name << ": " << af.message << '\n';
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[FAIL] " << t.name << ": exception: " << ex.what() << '\n';
            ++failed;
        } catch (...) {
            std::cout << "[FAIL] " << t.name << ": unknown exception\n";
            ++failed;
        }
    }

    std::cout << "\nResults: " << passed << " passed, " << failed << " failed"
              << " (out of " << (passed + failed) << " tests)\n";
    return (failed == 0) ? 0 : 1;
}
