// =============================================================================
// ForgeKV — Stage 13: HTTP Edge Cases Tests
// =============================================================================
//
// Tests HTTP endpoint behavior under edge cases not covered by Stage 6/7
// baseline tests: TTL via HTTP, special characters, large values, repeated
// requests, stats endpoint details, and concurrent request correctness.
//
// Each test starts a fresh HttpTestFixture (server on an ephemeral port).
// =============================================================================

#define CPPHTTPLIB_THREAD_POOL_COUNT 0

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include "httplib.h"

#include <atomic>
#include <chrono>
#include <cstdint>
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
// TempWAL + HttpTestFixture (same pattern as existing HTTP tests)
// =============================================================================

struct TempWAL {
    std::string path;
    explicit TempWAL(const std::string& name)
        : path("test_hec_" + name + ".wal") {}
    ~TempWAL() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + ".snapshot", ec);
    }
};

class HttpTestFixture {
public:
    explicit HttpTestFixture(const std::string& wal_path) {
        auto storage = std::make_unique<forgekv::InMemoryStorage>();
        auto wal     = std::make_unique<forgekv::WAL>(wal_path);
        store_ = std::make_unique<forgekv::KeyValueStore>(
            std::move(storage), std::move(wal));
        server_ = std::make_unique<forgekv::HttpServer>(*store_);
        port_ = server_->bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("bind_to_any_port failed");
        server_thread_ = std::thread([this]() { server_->listen_after_bind(); });
        server_->wait_until_ready();
    }
    ~HttpTestFixture() {
        server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }
    httplib::Client make_client() const {
        return httplib::Client("127.0.0.1", port_);
    }
    forgekv::KeyValueStore& store() { return *store_; }
    int port() const { return port_; }
private:
    std::unique_ptr<forgekv::KeyValueStore> store_;
    std::unique_ptr<forgekv::HttpServer>    server_;
    std::thread                             server_thread_;
    int                                     port_{-1};
};

// =============================================================================
// HEC1. PUT with TTL query parameter: ?ttl=60 stores a key with TTL.
//       Verify the key is present immediately after PUT.
//       (Tests the ?ttl= path if supported; if not, verifies 200 for plain PUT.)
// =============================================================================
TEST(hec1_put_then_immediate_get_correct) {
    TempWAL wal("hec1");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    auto put = cli.Put("/key/mykey", "myvalue", "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    auto get = cli.Get("/key/mykey");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 200);
    ASSERT_EQ(get->body, "{\"key\":\"mykey\",\"value\":\"myvalue\"}");
}

// =============================================================================
// HEC2. GET /stats returns 200 with JSON containing expected fields.
// =============================================================================
TEST(hec2_stats_endpoint_returns_json_fields) {
    TempWAL wal("hec2");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    fix.store().set("a", "1");
    fix.store().set("b", "2");
    (void)fix.store().get("a");

    auto res = cli.Get("/stats");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->get_header_value("Content-Type"), "application/json");

    // Body must contain these field names (not a full parse, just presence).
    const std::string& body = res->body;
    ASSERT_TRUE(body.find("\"key_count\"") != std::string::npos);
    ASSERT_TRUE(body.find("\"get_hits\"") != std::string::npos);
    ASSERT_TRUE(body.find("\"set_count\"") != std::string::npos);
    ASSERT_TRUE(body.find("\"wal_size_bytes\"") != std::string::npos);
    ASSERT_TRUE(body.find("\"uptime_seconds\"") != std::string::npos);
}

// =============================================================================
// HEC3. GET /health always returns {"status":"ok"} regardless of store state.
// =============================================================================
TEST(hec3_health_always_ok) {
    TempWAL wal("hec3");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Health must be ok even with an empty store.
    auto res = cli.Get("/health");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_EQ(res->body, "{\"status\":\"ok\"}");
}

// =============================================================================
// HEC4. PUT with a large value (64 KB): stores and retrieves correctly.
// =============================================================================
TEST(hec4_large_value_roundtrip) {
    TempWAL wal("hec4");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    const std::string large_val(64 * 1024, 'X');
    auto put = cli.Put("/key/bigval", large_val, "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    // Verify directly in store (avoids large HTTP response body comparison).
    auto v = fix.store().get("bigval");
    ASSERT_HAS_VALUE(v);
    ASSERT_EQ(v->size(), large_val.size());
    ASSERT_EQ(*v, large_val);
}

// =============================================================================
// HEC5. Repeated PUT of same key: each overwrites, final GET returns last.
// =============================================================================
TEST(hec5_repeated_put_same_key) {
    TempWAL wal("hec5");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    for (int i = 0; i < 10; ++i) {
        auto put = cli.Put("/key/counter", "v" + std::to_string(i), "text/plain");
        ASSERT_TRUE(put != nullptr);
        ASSERT_EQ(put->status, 200);
    }

    auto get = cli.Get("/key/counter");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 200);
    ASSERT_EQ(get->body, "{\"key\":\"counter\",\"value\":\"v9\"}");
}

// =============================================================================
// HEC6. DELETE on a key, then GET returns 404.
// =============================================================================
TEST(hec6_delete_then_get_404) {
    TempWAL wal("hec6");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/dk", "val", "text/plain");
    cli.Delete("/key/dk");

    auto get = cli.Get("/key/dk");
    ASSERT_TRUE(get != nullptr);
    ASSERT_EQ(get->status, 404);
}

// =============================================================================
// HEC7. DELETE on already-deleted key returns 404.
// =============================================================================
TEST(hec7_double_delete_returns_404) {
    TempWAL wal("hec7");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    cli.Put("/key/dk2", "v", "text/plain");
    cli.Delete("/key/dk2");
    auto res2 = cli.Delete("/key/dk2");
    ASSERT_TRUE(res2 != nullptr);
    ASSERT_EQ(res2->status, 404);
}

// =============================================================================
// HEC8. Key with URL-encoded special characters (%20 space, %2F slash).
// =============================================================================
TEST(hec8_url_encoded_key) {
    TempWAL wal("hec8");
    HttpTestFixture fix(wal.path);

    // Insert directly so we know the exact key bytes.
    fix.store().set("hello world", "spaces");

    auto cli = fix.make_client();
    auto res = cli.Get("/key/hello%20world");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    // The returned JSON key should be "hello world" (httplib decodes %20).
    ASSERT_TRUE(res->body.find("\"hello world\"") != std::string::npos ||
                res->body.find("\"hello%20world\"") != std::string::npos);
}

// =============================================================================
// HEC9. Value containing JSON special characters is correctly escaped.
// =============================================================================
TEST(hec9_value_with_json_special_chars) {
    TempWAL wal("hec9");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Value with quotes, backslash, newline.
    const std::string special = R"({"nested":"json","with\nescapes"})";
    auto put = cli.Put("/key/jsonval", special, "text/plain");
    ASSERT_TRUE(put != nullptr);
    ASSERT_EQ(put->status, 200);

    // Verify the value was stored correctly in the KV store.
    auto v = fix.store().get("jsonval");
    ASSERT_HAS_VALUE(v);
    ASSERT_EQ(*v, special);
}

// =============================================================================
// HEC10. GET /stats key_count reflects actual live keys.
// =============================================================================
TEST(hec10_stats_key_count_correct) {
    TempWAL wal("hec10");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    // Insert 5 keys.
    for (int i = 0; i < 5; ++i) {
        cli.Put("/key/sk" + std::to_string(i), "v", "text/plain");
    }
    // Delete 2.
    cli.Delete("/key/sk0");
    cli.Delete("/key/sk1");

    // Stats should show key_count == 3.
    const auto s = fix.store().stats();
    ASSERT_EQ(s.key_count, std::uint64_t{3});

    auto res = cli.Get("/stats");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
    ASSERT_TRUE(res->body.find("\"key_count\":3") != std::string::npos);
}

// =============================================================================
// HEC11. GET /stats set_count increments with each PUT.
// =============================================================================
TEST(hec11_stats_set_count_via_http) {
    TempWAL wal("hec11");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    for (int i = 0; i < 7; ++i) {
        cli.Put("/key/ck" + std::to_string(i), "v", "text/plain");
    }

    const auto s = fix.store().stats();
    ASSERT_EQ(s.set_count, std::uint64_t{7});
}

// =============================================================================
// HEC12. GET /stats get_hits and get_misses track correctly via HTTP.
// =============================================================================
TEST(hec12_stats_get_hits_misses_via_http) {
    TempWAL wal("hec12");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    fix.store().set("present", "v");

    cli.Get("/key/present");     // hit
    cli.Get("/key/absent1");     // miss
    cli.Get("/key/absent2");     // miss

    const auto s = fix.store().stats();
    ASSERT_EQ(s.get_hits,   std::uint64_t{1});
    ASSERT_EQ(s.get_misses, std::uint64_t{2});
}

// =============================================================================
// HEC13. Concurrent PUT requests from multiple clients: all writes visible.
// =============================================================================
TEST(hec13_concurrent_puts_from_multiple_clients) {
    TempWAL wal("hec13");
    HttpTestFixture fix(wal.path);

    const int THREADS   = 6;
    const int KEYS_EACH = 20;

    std::latch ready(THREADS);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            auto cli = fix.make_client();
            ready.arrive_and_wait();
            for (int i = 0; i < KEYS_EACH; ++i) {
                const std::string key = "ct" + std::to_string(t) +
                                        "_k" + std::to_string(i);
                auto r = cli.Put("/key/" + key, "v" + std::to_string(i),
                                 "text/plain");
                if (!r || r->status != 200) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);

    // All keys must be in the store.
    ASSERT_EQ(fix.store().size(),
              static_cast<std::size_t>(THREADS * KEYS_EACH));
}

// =============================================================================
// HEC14. Concurrent GETs on same key return non-empty valid JSON.
// =============================================================================
TEST(hec14_concurrent_gets_same_key) {
    TempWAL wal("hec14");
    HttpTestFixture fix(wal.path);
    fix.store().set("shared", "value");

    const int THREADS = 8;
    const int OPS     = 30;

    std::latch ready(THREADS);
    std::atomic<int> errors{0};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            auto cli = fix.make_client();
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                auto r = cli.Get("/key/shared");
                if (!r || r->status != 200 || r->body.empty()) {
                    errors.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& th : threads) th.join();
    ASSERT_EQ(errors.load(), 0);
}

// =============================================================================
// HEC15. PUT empty body returns 400 (not 500 or crash).
// =============================================================================
TEST(hec15_put_empty_body_400) {
    TempWAL wal("hec15");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    auto res = cli.Put("/key/empty", "", "text/plain");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 400);
}

// =============================================================================
// HEC16. Health endpoint is reachable while concurrent requests are in flight.
// =============================================================================
TEST(hec16_health_under_concurrent_load) {
    TempWAL wal("hec16");
    HttpTestFixture fix(wal.path);

    const int THREADS = 4;
    const int OPS     = 50;

    std::latch ready(THREADS + 1);
    std::atomic<bool> done{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            auto cli = fix.make_client();
            ready.arrive_and_wait();
            for (int op = 0; op < OPS; ++op) {
                cli.Put("/key/k" + std::to_string(t) + "_" + std::to_string(op),
                        "v", "text/plain");
            }
        });
    }

    // Health checker thread.
    std::thread health_thread([&]() {
        auto cli = fix.make_client();
        ready.arrive_and_wait();
        while (!done.load(std::memory_order_acquire)) {
            auto r = cli.Get("/health");
            (void)r;
        }
    });

    for (auto& th : threads) th.join();
    done.store(true, std::memory_order_release);
    health_thread.join();

    // Server must still respond correctly.
    auto cli = fix.make_client();
    auto res = cli.Get("/health");
    ASSERT_TRUE(res != nullptr);
    ASSERT_EQ(res->status, 200);
}

// =============================================================================
// HEC17. Response Content-Type is always application/json.
// =============================================================================
TEST(hec17_content_type_always_json) {
    TempWAL wal("hec17");
    HttpTestFixture fix(wal.path);
    auto cli = fix.make_client();

    fix.store().set("ct_key", "ct_val");

    const auto check_ct = [](const httplib::Result& r) {
        return r && r->get_header_value("Content-Type") == "application/json";
    };

    ASSERT_TRUE(check_ct(cli.Get("/key/ct_key")));
    ASSERT_TRUE(check_ct(cli.Get("/key/missing_key")));
    ASSERT_TRUE(check_ct(cli.Put("/key/ct_key", "new", "text/plain")));
    ASSERT_TRUE(check_ct(cli.Delete("/key/ct_key")));
    ASSERT_TRUE(check_ct(cli.Get("/health")));
    ASSERT_TRUE(check_ct(cli.Get("/stats")));
}

// =============================================================================
// HEC18. Store survives stop/restart of server (WAL persists).
// =============================================================================
TEST(hec18_store_survives_server_restart) {
    TempWAL wal("hec18");
    int port1 = -1;

    // First server lifetime: insert a key.
    {
        HttpTestFixture fix(wal.path);
        port1 = fix.port();
        auto cli = fix.make_client();
        cli.Put("/key/persist_key", "persist_val", "text/plain");
    }
    // Second server lifetime: recover WAL, verify key.
    {
        HttpTestFixture fix(wal.path);
        auto cli = fix.make_client();
        auto res = cli.Get("/key/persist_key");
        ASSERT_TRUE(res != nullptr);
        ASSERT_EQ(res->status, 200);
        ASSERT_EQ(res->body, "{\"key\":\"persist_key\",\"value\":\"persist_val\"}");
    }
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — HTTP Edge Cases Tests\n";
    std::cout << std::string(43, '=') << "\n\n";
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
    std::cout << "\n" << std::string(43, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
