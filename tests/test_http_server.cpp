// =============================================================================
// ForgeKV — Stage 6 + Stage 7: HTTP Integration Tests
// =============================================================================
//
// Tests the HttpServer REST API end-to-end using cpp-httplib's Client.
//
// Stage 6 tests: single-client correctness of all four REST endpoints.
// Stage 7 tests: concurrent multi-client correctness under the thread pool.
//
// Test server lifecycle
// ---------------------
//   Each test fixture (HttpTestFixture) starts a ForgeKV HTTP server on a
//   freshly allocated ephemeral port (bind_to_any_port + listen_after_bind).
//   The server runs on a dedicated std::thread for the duration of the fixture.
//   After each fixture the server is stopped and the thread is joined.
//
//   Stage 7: The server uses cpp-httplib's default ThreadPool (no InlineTaskQueue
//   override), so multiple requests are dispatched to worker threads concurrently.
//   KeyValueStore's std::shared_mutex ensures thread safety.
//
// Port selection
// --------------
//   bind_to_any_port("127.0.0.1") returns the OS-assigned port.
//   listen_after_bind() starts accepting on that port.
//   wait_until_ready() is called before the first client request so that
//   the server is guaranteed to be listening.
//
// Storage isolation
// -----------------
//   Each test group gets its own KeyValueStore backed by a temp WAL file,
//   preventing state bleed between test groups. The temp WAL is deleted on
//   scope exit (TempWAL RAII guard).
//
// Test harness
// ------------
//   Uses the same custom harness (TEST / ASSERT_*) as test_kv_store.cpp so
//   that both binaries share the same runner style. The macros are redefined
//   locally — they are not shared via a header.
//
// cpp-httplib client usage
// ------------------------
//   httplib::Client connects to "127.0.0.1" on the ephemeral port.
//   All responses are verified for status code and body.
// =============================================================================

#define CPPHTTPLIB_THREAD_POOL_COUNT 0  // suppress the thread pool in this TU
                                         // (httplib still compiles cleanly)

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

// httplib client is in the same single-header
#include "httplib.h"

#include <atomic>
#include <chrono>
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
// Minimal test harness (same style as test_kv_store.cpp)
// =============================================================================

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

struct AssertionFailure {
    std::string message;
};

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            throw AssertionFailure{"ASSERT_TRUE failed: " #cond \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_FALSE(cond) \
    do { \
        if ((cond)) { \
            throw AssertionFailure{"ASSERT_FALSE failed: " #cond \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_HAS_VALUE(opt) \
    do { \
        if (!(opt).has_value()) { \
            throw AssertionFailure{"ASSERT_HAS_VALUE failed: " #opt \
                " is empty (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_NO_VALUE(opt) \
    do { \
        if ((opt).has_value()) { \
            throw AssertionFailure{"ASSERT_NO_VALUE failed: " #opt \
                " has a value (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar registrar_##name{#name, test_##name}; \
    static void test_##name()

// =============================================================================
// TempWAL — RAII guard that creates a unique temp WAL path and deletes it
// =============================================================================

struct TempWAL {
    std::string path;
    explicit TempWAL(const std::string& name)
        : path("test_http_" + name + ".wal") {}
    ~TempWAL() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// =============================================================================
// HttpTestFixture — starts/stops a ForgeKV HTTP server for one group of tests
// =============================================================================
//
// Usage:
//
//   TempWAL wal("my_test");
//   HttpTestFixture fix(wal.path);
//
//   // fix.client() returns a connected httplib::Client
//   // fix.store()  returns the KeyValueStore (for direct verification)

class HttpTestFixture {
public:
    explicit HttpTestFixture(const std::string& wal_path) {
        // Build the KeyValueStore using a temp WAL.
        auto storage = std::make_unique<forgekv::InMemoryStorage>();
        auto wal     = std::make_unique<forgekv::WAL>(wal_path);
        store_ = std::make_unique<forgekv::KeyValueStore>(
            std::move(storage), std::move(wal));

        // Build the HTTP server (non-owning reference to the store).
        server_ = std::make_unique<forgekv::HttpServer>(*store_);

        // Bind to an ephemeral port before starting the server thread.
        port_ = server_->bind_to_any_port("127.0.0.1");
        if (port_ < 0) {
            throw std::runtime_error("HttpTestFixture: bind_to_any_port failed");
        }

        // Start listening on the background thread.
        server_thread_ = std::thread([this]() {
            server_->listen_after_bind();
        });

        // Wait until the server is accepting connections.
        server_->wait_until_ready();
    }

    ~HttpTestFixture() {
        server_->stop();
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    // Returns a client connected to the test server.
    httplib::Client make_client() const {
        return httplib::Client("127.0.0.1", port_);
    }

    // Direct access to the store for post-request verification.
    forgekv::KeyValueStore& store() { return *store_; }

    int port() const { return port_; }

private:
    std::unique_ptr<forgekv::KeyValueStore> store_;
    std::unique_ptr<forgekv::HttpServer>    server_;
    std::thread                             server_thread_;
    int                                     port_{-1};
};

// =============================================================================
// Tests — GET /key/:key
// =============================================================================

TEST(get_existing_key_returns_200_with_json) {
    TempWAL wal("get_existing");
    HttpTestFixture fix(wal.path);

    fix.store().set("hello", "world");

    auto cli = fix.make_client();
    auto res = cli.Get("/key/hello");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->get_header_value("Content-Type"), "application/json");
    ASSERT_EQ(res->body, "{\"key\":\"hello\",\"value\":\"world\"}");
}

TEST(get_missing_key_returns_404) {
    TempWAL wal("get_missing");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    auto res = cli.Get("/key/does_not_exist");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 404);
    ASSERT_EQ(res->body, "{\"error\":\"key not found\"}");
}

// =============================================================================
// Tests — PUT /key/:key
// =============================================================================

TEST(put_new_key_returns_200) {
    TempWAL wal("put_new");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    auto res = cli.Put("/key/name", "Vishnu", "text/plain");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->body, "{\"status\":\"ok\"}");
}

TEST(put_new_key_persists_in_store) {
    TempWAL wal("put_persist");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    cli.Put("/key/color", "blue", "text/plain");

    // Verify directly in the store (no second HTTP round-trip required).
    auto val = fix.store().get("color");
    ASSERT_HAS_VALUE(val);
    ASSERT_EQ(val.value(), "blue");
}

TEST(put_overwrite_existing_key_returns_200_with_updated_value) {
    TempWAL wal("put_overwrite");
    HttpTestFixture fix(wal.path);

    fix.store().set("lang", "C");

    auto cli = fix.make_client();
    auto res = cli.Put("/key/lang", "C++20", "text/plain");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);

    // Confirm updated value via GET.
    auto get_res = cli.Get("/key/lang");
    ASSERT_TRUE(get_res != nullptr);
    ASSERT_EQ(get_res->status, 200);
    ASSERT_EQ(get_res->body, "{\"key\":\"lang\",\"value\":\"C++20\"}");
}

TEST(put_empty_body_returns_400) {
    TempWAL wal("put_empty");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    // Send PUT with empty body.
    auto res = cli.Put("/key/empty_test", "", "text/plain");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 400);
    ASSERT_EQ(res->body, "{\"error\":\"value cannot be empty\"}");
}

// =============================================================================
// Tests — DELETE /key/:key
// =============================================================================

TEST(delete_existing_key_returns_200) {
    TempWAL wal("delete_existing");
    HttpTestFixture fix(wal.path);

    fix.store().set("to_delete", "yes");

    auto cli = fix.make_client();
    auto res = cli.Delete("/key/to_delete");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->body, "{\"status\":\"ok\"}");
}

TEST(delete_existing_key_removes_from_store) {
    TempWAL wal("delete_removes");
    HttpTestFixture fix(wal.path);

    fix.store().set("temp_key", "temp_value");

    auto cli = fix.make_client();
    cli.Delete("/key/temp_key");

    // Confirm the key is gone.
    ASSERT_FALSE(fix.store().exists("temp_key"));

    // Confirm GET now returns 404.
    auto get_res = cli.Get("/key/temp_key");
    ASSERT_TRUE(get_res != nullptr);
    ASSERT_EQ(get_res->status, 404);
}

TEST(delete_missing_key_returns_404) {
    TempWAL wal("delete_missing");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    auto res = cli.Delete("/key/no_such_key");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 404);
    ASSERT_EQ(res->body, "{\"error\":\"key not found\"}");
}

// =============================================================================
// Tests — GET /health
// =============================================================================

TEST(health_returns_200_with_status_ok) {
    TempWAL wal("health");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();
    auto res = cli.Get("/health");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->get_header_value("Content-Type"), "application/json");
    ASSERT_EQ(res->body, "{\"status\":\"ok\"}");
}

// =============================================================================
// Tests — JSON escaping
// =============================================================================

TEST(put_and_get_key_with_quote_character) {
    TempWAL wal("json_quote");
    HttpTestFixture fix(wal.path);

    // Key contains a double-quote — must be escaped in JSON response.
    // Use PUT via direct store insertion to set the key with a quote.
    fix.store().set("say\"hi", "hello");

    auto cli    = fix.make_client();
    // Encode the quote in the URL path as %22.
    auto res = cli.Get("/key/say%22hi");

    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    // The JSON value for key should have \" escaped.
    ASSERT_EQ(res->body, "{\"key\":\"say\\\"hi\",\"value\":\"hello\"}");
}

TEST(put_and_get_value_with_backslash) {
    TempWAL wal("json_backslash");
    HttpTestFixture fix(wal.path);

    // Value contains a backslash — must be \\ in JSON.
    auto cli = fix.make_client();
    auto put = cli.Put("/key/path_key", "C:\\Users\\Vishnu", "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    auto get = cli.Get("/key/path_key");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 200);
    // Backslash should be doubled in JSON output.
    ASSERT_EQ(get->body, "{\"key\":\"path_key\",\"value\":\"C:\\\\Users\\\\Vishnu\"}");
}

TEST(put_and_get_value_with_newline_and_tab) {
    TempWAL wal("json_newline_tab");
    HttpTestFixture fix(wal.path);

    // Value contains newline and tab characters.
    auto cli = fix.make_client();
    auto put = cli.Put("/key/multiline", "line1\nline2\ttabbed", "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    auto get = cli.Get("/key/multiline");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 200);
    // \n → \\n, \t → \\t in JSON.
    ASSERT_EQ(get->body, "{\"key\":\"multiline\",\"value\":\"line1\\nline2\\ttabbed\"}");
}

// =============================================================================
// Tests — end-to-end round-trip
// =============================================================================

TEST(round_trip_set_get_delete) {
    TempWAL wal("roundtrip");
    HttpTestFixture fix(wal.path);

    auto cli = fix.make_client();

    // PUT
    auto put = cli.Put("/key/x", "42", "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    // GET — should return value
    auto get = cli.Get("/key/x");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 200);
    ASSERT_EQ(get->body, "{\"key\":\"x\",\"value\":\"42\"}");

    // DELETE
    auto del = cli.Delete("/key/x");
    ASSERT_TRUE(del != nullptr);
    ASSERT_EQ(del->status, 200);

    // GET after delete — should 404
    auto get2 = cli.Get("/key/x");
    ASSERT_TRUE(get2 != nullptr);
    ASSERT_EQ(get2->status, 404);
}

// =============================================================================
// Tests — Stage 7: HTTP Concurrency
// =============================================================================
//
// Verifies that the HttpServer can handle multiple concurrent requests
// correctly when backed by a thread-safe KeyValueStore.
//
// Strategy:
//   - Launch a single test server (ephemeral port).
//   - Fire N client threads simultaneously (via std::latch).
//   - Each thread performs PUT then GET operations on distinct key namespaces.
//   - After all threads finish, verify every key has the correct final value.
//   - Also verify that concurrent GETs on the same key do not produce garbage.
//
// This test does NOT rely on timing to prove concurrency — it verifies
// correctness under concurrent load.
// =============================================================================

// ---------------------------------------------------------------------------
// S7-HTTP-1. Concurrent PUTs from multiple clients — all writes survive
//
//   T client threads each write K distinct keys.
//   After all threads join, a single-threaded verification pass reads every
//   key via HTTP GET and confirms the expected value.
// ---------------------------------------------------------------------------
TEST(s7_http_concurrent_puts_distinct_keys) {
    TempWAL wal("s7_http_puts");
    HttpTestFixture fix(wal.path);

    const int THREADS   = 6;
    const int KEYS_EACH = 20;

    std::latch ready(THREADS);
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();

            auto cli = fix.make_client();
            for (int i = 0; i < KEYS_EACH; ++i) {
                const std::string key = "/key/t" + std::to_string(t)
                                      + "_k" + std::to_string(i);
                const std::string val = "v_t" + std::to_string(t)
                                      + "_k" + std::to_string(i);
                auto res = cli.Put(key, val, "text/plain");
                if (!res || res->status != 200) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // Single-threaded verification.
    auto cli = fix.make_client();
    for (int t = 0; t < THREADS; ++t) {
        for (int i = 0; i < KEYS_EACH; ++i) {
            const std::string key = "/key/t" + std::to_string(t)
                                  + "_k" + std::to_string(i);
            auto res = cli.Get(key);
            ASSERT_TRUE(res != nullptr);
            ASSERT_EQ(res->status, 200);
        }
    }

    ASSERT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
// S7-HTTP-2. Concurrent GETs — correct values, no corrupted responses
//
//   Seed K keys directly in the store.
//   Launch T reader threads, each making R GET requests.
//   Every response must be 200 with a valid JSON body.
// ---------------------------------------------------------------------------
TEST(s7_http_concurrent_gets) {
    TempWAL wal("s7_http_gets");
    HttpTestFixture fix(wal.path);

    const int KEYS    = 10;
    const int THREADS = 6;
    const int ROUNDS  = 20;

    // Pre-seed keys directly.
    for (int i = 0; i < KEYS; ++i) {
        fix.store().set("gk" + std::to_string(i),
                        "gv" + std::to_string(i));
    }

    std::latch ready(THREADS);
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();

            auto cli = fix.make_client();
            for (int r = 0; r < ROUNDS; ++r) {
                for (int i = 0; i < KEYS; ++i) {
                    const std::string path = "/key/gk" + std::to_string(i);
                    auto res = cli.Get(path);
                    if (!res || res->status != 200) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Response body must contain the key.
                    if (res && res->body.find("gk" + std::to_string(i))
                                == std::string::npos) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    ASSERT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
// S7-HTTP-3. Mixed concurrent PUT + GET + DELETE — server stays consistent
//
//   Writers perform PUT/DELETE in a loop.
//   Readers perform GET in a loop.
//   No crashes, no 500 responses, no malformed JSON.
// ---------------------------------------------------------------------------
TEST(s7_http_concurrent_mixed_put_get_delete) {
    TempWAL wal("s7_http_mixed");
    HttpTestFixture fix(wal.path);

    const int WRITE_THREADS = 3;
    const int READ_THREADS  = 3;
    const int KEYS          = 5;
    const int OPS_EACH      = 30;

    // Pre-seed.
    for (int i = 0; i < KEYS; ++i) {
        fix.store().set("mk" + std::to_string(i), "initial");
    }

    std::latch ready(WRITE_THREADS + READ_THREADS);
    std::atomic<int> server_errors{0}; // counts any 5xx response

    std::vector<std::thread> threads;
    threads.reserve(WRITE_THREADS + READ_THREADS);

    // Write threads: alternate PUT and DELETE.
    for (int t = 0; t < WRITE_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();

            auto cli = fix.make_client();
            for (int op = 0; op < OPS_EACH; ++op) {
                const int ki = op % KEYS;
                const std::string path = "/key/mk" + std::to_string(ki);
                if (op % 3 == 0) {
                    // DELETE
                    auto res = cli.Delete(path);
                    if (res && res->status >= 500) {
                        server_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                } else {
                    // PUT
                    const std::string val = "w" + std::to_string(t)
                                           + "_op" + std::to_string(op);
                    auto res = cli.Put(path, val, "text/plain");
                    if (res && res->status >= 500) {
                        server_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Read threads: GET repeatedly.
    for (int t = 0; t < READ_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();

            auto cli = fix.make_client();
            for (int op = 0; op < OPS_EACH; ++op) {
                const int ki = op % KEYS;
                const std::string path = "/key/mk" + std::to_string(ki);
                auto res = cli.Get(path);
                if (res && res->status >= 500) {
                    server_errors.fetch_add(1, std::memory_order_relaxed);
                }
                // 200 or 404 are both valid (key may have been deleted).
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // No 5xx responses.
    ASSERT_EQ(server_errors.load(), 0);

    // Health endpoint must still respond correctly.
    auto cli = fix.make_client();
    auto health = cli.Get("/health");
    ASSERT_TRUE(health != nullptr);
    ASSERT_EQ(health->status, 200);
    ASSERT_EQ(health->body, "{\"status\":\"ok\"}");
}

// =============================================================================
// Test runner
// =============================================================================

// =============================================================================
// Stage 10 — HTTP TTL Tests (X-TTL-Seconds header)
// =============================================================================
//
// H10-A. PUT without X-TTL-Seconds stores key permanently.
// H10-B. PUT with valid X-TTL-Seconds stores key with TTL.
// H10-C. GET of expired key returns 404.
// H10-D. PUT with X-TTL-Seconds = 0 returns 400.
// H10-E. PUT with negative X-TTL-Seconds returns 400.
// H10-F. PUT with non-numeric X-TTL-Seconds returns 400.
// H10-G. PUT with very large X-TTL-Seconds succeeds.
// H10-H. Overwrite expiring key with normal PUT makes it permanent.
// =============================================================================

// ---------------------------------------------------------------------------
// H10-A. PUT without X-TTL-Seconds → permanent key.
// ---------------------------------------------------------------------------
TEST(h10_put_no_ttl_header_permanent) {
    TempWAL wal("h10a");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    auto res = cli.Put("/key/mykey", "myvalue", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    // Key should be permanent.
    ASSERT_EQ(fix.store().ttl("mykey"), forgekv::kTtlPermanent);
}

// ---------------------------------------------------------------------------
// H10-B. PUT with valid X-TTL-Seconds stores key with TTL.
// ---------------------------------------------------------------------------
TEST(h10_put_with_ttl_header_stores_ttl) {
    TempWAL wal("h10b");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    httplib::Headers headers{{"X-TTL-Seconds", "60"}};
    auto res = cli.Put("/key/session", headers, "token123", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    // Key should have a TTL close to 60 seconds.
    const double remaining = fix.store().ttl("session");
    ASSERT_TRUE(remaining > 0.0 && remaining <= 60.0);
}

// ---------------------------------------------------------------------------
// H10-C. GET of expired key returns 404.
// ---------------------------------------------------------------------------
TEST(h10_get_expired_key_returns_404) {
    TempWAL wal("h10c");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Set a very short TTL via direct store call (10ms is too short for HTTP
    // round-trip, so use the store directly).
    fix.store().set_with_ttl("quick_exp", "val", 0.010);

    // Wait for expiry.
    std::this_thread::sleep_for(std::chrono::milliseconds{50});

    auto res = cli.Get("/key/quick_exp");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 404);
}

// ---------------------------------------------------------------------------
// H10-D. PUT with X-TTL-Seconds = 0 returns 400.
// ---------------------------------------------------------------------------
TEST(h10_put_ttl_zero_returns_400) {
    TempWAL wal("h10d");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    httplib::Headers headers{{"X-TTL-Seconds", "0"}};
    auto res = cli.Put("/key/k", headers, "v", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
}

// ---------------------------------------------------------------------------
// H10-E. PUT with negative X-TTL-Seconds returns 400.
// ---------------------------------------------------------------------------
TEST(h10_put_negative_ttl_returns_400) {
    TempWAL wal("h10e");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    httplib::Headers headers{{"X-TTL-Seconds", "-10"}};
    auto res = cli.Put("/key/k", headers, "v", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
}

// ---------------------------------------------------------------------------
// H10-F. PUT with non-numeric X-TTL-Seconds returns 400.
// ---------------------------------------------------------------------------
TEST(h10_put_non_numeric_ttl_returns_400) {
    TempWAL wal("h10f");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    httplib::Headers headers{{"X-TTL-Seconds", "abc"}};
    auto res = cli.Put("/key/k", headers, "v", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 400);
}

// ---------------------------------------------------------------------------
// H10-G. PUT with very large X-TTL-Seconds succeeds (no overflow).
// ---------------------------------------------------------------------------
TEST(h10_put_large_ttl_succeeds) {
    TempWAL wal("h10g");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    httplib::Headers headers{{"X-TTL-Seconds", "86400"}};  // 1 day
    auto res = cli.Put("/key/longkey", headers, "longval", "text/plain");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    // Key should exist with a large TTL.
    const double remaining = fix.store().ttl("longkey");
    ASSERT_TRUE(remaining > 0.0 && remaining <= 86400.0);
}

// ---------------------------------------------------------------------------
// H10-H. Overwrite expiring key with normal PUT makes it permanent.
// ---------------------------------------------------------------------------
TEST(h10_overwrite_expiring_with_normal_put) {
    TempWAL wal("h10h");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // First set with TTL.
    httplib::Headers ttl_headers{{"X-TTL-Seconds", "5"}};
    {
        auto res = cli.Put("/key/key1", ttl_headers, "val1", "text/plain");
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
    }
    ASSERT_TRUE(fix.store().ttl("key1") > 0.0);

    // Overwrite without TTL header.
    {
        auto res = cli.Put("/key/key1", "new_permanent_value", "text/plain");
        ASSERT_TRUE(res);
        ASSERT_EQ(res->status, 200);
    }
    // Now the key should be permanent.
    ASSERT_EQ(fix.store().ttl("key1"), forgekv::kTtlPermanent);
    ASSERT_EQ(*fix.store().get("key1"), "new_permanent_value");
}

// =============================================================================
// End of Stage 10 HTTP tests
// =============================================================================

// =============================================================================
// Stage 11 HTTP Tests — GET /stats
// =============================================================================
//
//   H11-A.  GET /stats returns HTTP 200.
//   H11-B.  GET /stats response contains all required JSON fields.
//   H11-C.  GET /stats reflects correct live key count after PUT.
//   H11-D.  GET /stats get_hits incremented after successful GET /key/:key.
//   H11-E.  GET /stats get_misses incremented after GET /key/:key miss.
//   H11-F.  GET /stats set_count incremented after PUT /key/:key.
//   H11-G.  GET /stats delete_count incremented after DELETE /key/:key.
//   H11-H.  GET /stats uptime_seconds is a positive number.
//   H11-I.  GET /stats wal_size_bytes positive after writes.
//   H11-J.  GET /stats last_snapshot_time_us is 0 before snapshot,
//           positive after snapshot.
//   H11-K.  GET /stats does not return 404 or 500 on empty store.
//   H11-L.  GET /stats JSON is well-formed (parseable, no trailing text).
// =============================================================================

// Helper: simple JSON field extractor — finds "field":value and returns
// the value as a string.  Enough for the integer / double fields we need.
static std::string extract_json_field(const std::string& body,
                                       const std::string& field)
{
    const std::string needle = "\"" + field + "\":";
    const auto pos = body.find(needle);
    if (pos == std::string::npos) return "";
    std::size_t start = pos + needle.size();
    // Skip optional leading quote (for string values).
    const bool is_string = (start < body.size() && body[start] == '"');
    if (is_string) ++start;
    std::size_t end = start;
    while (end < body.size()) {
        const char c = body[end];
        if (is_string) {
            if (c == '"') break;
        } else {
            if (c == ',' || c == '}') break;
        }
        ++end;
    }
    return body.substr(start, end - start);
}

// ---------------------------------------------------------------------------
// H11-A. GET /stats returns HTTP 200.
// ---------------------------------------------------------------------------
TEST(h11_stats_returns_200) {
    TempWAL wal("h11a");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();
    auto res = cli.Get("/stats");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
}

// ---------------------------------------------------------------------------
// H11-B. GET /stats response contains all required JSON fields.
// ---------------------------------------------------------------------------
TEST(h11_stats_contains_all_required_fields) {
    TempWAL wal("h11b");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();
    auto res = cli.Get("/stats");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);

    const std::string& body = res->body;
    ASSERT_FALSE(extract_json_field(body, "key_count").empty());
    ASSERT_FALSE(extract_json_field(body, "get_hits").empty());
    ASSERT_FALSE(extract_json_field(body, "get_misses").empty());
    ASSERT_FALSE(extract_json_field(body, "set_count").empty());
    ASSERT_FALSE(extract_json_field(body, "delete_count").empty());
    ASSERT_FALSE(extract_json_field(body, "ttl_set_count").empty());
    ASSERT_FALSE(extract_json_field(body, "expired_count").empty());
    ASSERT_FALSE(extract_json_field(body, "wal_size_bytes").empty());
    ASSERT_FALSE(extract_json_field(body, "uptime_seconds").empty());
    ASSERT_FALSE(extract_json_field(body, "last_snapshot_time_us").empty());
}

// ---------------------------------------------------------------------------
// H11-C. GET /stats reflects correct live key count after PUT.
// ---------------------------------------------------------------------------
TEST(h11_stats_key_count_reflects_puts) {
    TempWAL wal("h11c");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Initially 0.
    {
        auto res = cli.Get("/stats");
        ASSERT_TRUE(res);
        ASSERT_EQ(extract_json_field(res->body, "key_count"), "0");
    }

    // PUT two keys.
    cli.Put("/key/alpha", "val_a", "text/plain");
    cli.Put("/key/beta",  "val_b", "text/plain");

    {
        auto res = cli.Get("/stats");
        ASSERT_TRUE(res);
        ASSERT_EQ(extract_json_field(res->body, "key_count"), "2");
    }

    // DELETE one.
    cli.Delete("/key/alpha");

    {
        auto res = cli.Get("/stats");
        ASSERT_TRUE(res);
        ASSERT_EQ(extract_json_field(res->body, "key_count"), "1");
    }
}

// ---------------------------------------------------------------------------
// H11-D. GET /stats get_hits incremented after successful GET /key/:key.
// ---------------------------------------------------------------------------
TEST(h11_stats_get_hits_incremented) {
    TempWAL wal("h11d");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/hit_key", "hit_val", "text/plain");

    // GET an existing key → should increment get_hits.
    cli.Get("/key/hit_key");
    cli.Get("/key/hit_key");

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string hits = extract_json_field(stats_res->body, "get_hits");
    ASSERT_TRUE(std::stoul(hits) >= 2u);
}

// ---------------------------------------------------------------------------
// H11-E. GET /stats get_misses incremented after GET /key/:key miss.
// ---------------------------------------------------------------------------
TEST(h11_stats_get_misses_incremented) {
    TempWAL wal("h11e");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // GET a missing key.
    cli.Get("/key/no_such_key");
    cli.Get("/key/also_missing");

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string misses = extract_json_field(stats_res->body, "get_misses");
    ASSERT_TRUE(std::stoul(misses) >= 2u);
}

// ---------------------------------------------------------------------------
// H11-F. GET /stats set_count incremented after PUT /key/:key.
// ---------------------------------------------------------------------------
TEST(h11_stats_set_count_incremented) {
    TempWAL wal("h11f");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/s1", "v1", "text/plain");
    cli.Put("/key/s2", "v2", "text/plain");
    cli.Put("/key/s3", "v3", "text/plain");

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string sc = extract_json_field(stats_res->body, "set_count");
    ASSERT_TRUE(std::stoul(sc) >= 3u);
}

// ---------------------------------------------------------------------------
// H11-G. GET /stats delete_count incremented after DELETE /key/:key.
// ---------------------------------------------------------------------------
TEST(h11_stats_delete_count_incremented) {
    TempWAL wal("h11g");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/d1", "v1", "text/plain");
    cli.Put("/key/d2", "v2", "text/plain");
    cli.Delete("/key/d1");

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string dc = extract_json_field(stats_res->body, "delete_count");
    ASSERT_TRUE(std::stoul(dc) >= 1u);
}

// ---------------------------------------------------------------------------
// H11-H. GET /stats uptime_seconds is a positive number.
// ---------------------------------------------------------------------------
TEST(h11_stats_uptime_is_positive) {
    TempWAL wal("h11h");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string uptime_str =
        extract_json_field(stats_res->body, "uptime_seconds");
    ASSERT_FALSE(uptime_str.empty());
    const double uptime = std::stod(uptime_str);
    ASSERT_TRUE(uptime >= 0.0);
}

// ---------------------------------------------------------------------------
// H11-I. GET /stats wal_size_bytes positive after writes.
// ---------------------------------------------------------------------------
TEST(h11_stats_wal_size_positive_after_writes) {
    TempWAL wal("h11i");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/walkey", "walval", "text/plain");

    auto stats_res = cli.Get("/stats");
    ASSERT_TRUE(stats_res);
    const std::string ws = extract_json_field(stats_res->body, "wal_size_bytes");
    ASSERT_TRUE(std::stoul(ws) > 0u);
}

// ---------------------------------------------------------------------------
// H11-J. last_snapshot_time_us is 0 before snapshot; positive after.
// ---------------------------------------------------------------------------
TEST(h11_stats_snapshot_time_zero_then_positive) {
    TempWAL wal("h11j");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Before snapshot.
    {
        auto stats_res = cli.Get("/stats");
        ASSERT_TRUE(stats_res);
        const std::string t =
            extract_json_field(stats_res->body, "last_snapshot_time_us");
        ASSERT_EQ(t, "0");
    }

    // Trigger a snapshot via the store directly.
    fix.store().snapshot();

    // After snapshot.
    {
        auto stats_res = cli.Get("/stats");
        ASSERT_TRUE(stats_res);
        const std::string t =
            extract_json_field(stats_res->body, "last_snapshot_time_us");
        ASSERT_TRUE(std::stoull(t) > 0u);
    }
}

// ---------------------------------------------------------------------------
// H11-K. GET /stats does not return 404 or 500 on empty store.
// ---------------------------------------------------------------------------
TEST(h11_stats_works_on_empty_store) {
    TempWAL wal("h11k");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();
    auto res = cli.Get("/stats");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    // key_count should be 0.
    ASSERT_EQ(extract_json_field(res->body, "key_count"), "0");
}

// ---------------------------------------------------------------------------
// H11-L. GET /stats JSON is well-formed: starts with '{', ends with '}'.
// ---------------------------------------------------------------------------
TEST(h11_stats_json_well_formed) {
    TempWAL wal("h11l");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();
    auto res = cli.Get("/stats");
    ASSERT_TRUE(res);
    ASSERT_EQ(res->status, 200);
    ASSERT_FALSE(res->body.empty());
    ASSERT_EQ(res->body.front(), '{');
    ASSERT_EQ(res->body.back(),  '}');
}

// =============================================================================
// End of Stage 11 HTTP tests
// =============================================================================

int main() {
    const auto& tests = test_registry();

    int passed = 0;
    int failed  = 0;

    std::cout << "[==========] Running " << tests.size()
              << " HTTP test(s).\n";

    for (const auto& tc : tests) {
        try {
            tc.fn();
            std::cout << "[ PASS ] " << tc.name << "\n";
            ++passed;
        } catch (const AssertionFailure& af) {
            std::cout << "[ FAIL ] " << tc.name << " — " << af.message << "\n";
            ++failed;
        } catch (const std::exception& ex) {
            std::cout << "[ FAIL ] " << tc.name
                      << " — unexpected exception: " << ex.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "[ FAIL ] " << tc.name << " — unknown exception\n";
            ++failed;
        }
    }

    std::cout << "[==========] " << passed << " passed, " << failed
              << " failed.\n";

    return (failed == 0) ? 0 : 1;
}
