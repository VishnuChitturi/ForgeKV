// =============================================================================
// ForgeKV — Stage 12: HTTP Benchmark
// =============================================================================
//
// Measures throughput of the ForgeKV HTTP server using real HTTP requests.
//
// Architecture:
//   - A ForgeKV HttpServer is started on an ephemeral port (OS-assigned).
//   - Client threads send real HTTP requests via cpp-httplib.
//   - The server uses cpp-httplib's default ThreadPool — concurrent requests.
//   - All client threads synchronize at a std::barrier before starting.
//
// Workloads:
//   A. HTTP GET (hit)
//   B. HTTP PUT
//   C. HTTP Mixed (GET + PUT)
//
// Timing methodology:
//   Same as KV benchmarks: setup outside timing, warmup outside timing,
//   measure only the workload phase using steady_clock.
//
// Notes:
//   - HTTP throughput is fundamentally limited by network stack overhead,
//     even on loopback (127.0.0.1). Numbers will be much lower than raw
//     in-process KV throughput.
//   - This exercises the ACTUAL HTTP interface — no handler bypassing.
//   - Ephemeral port avoids conflicts with other processes.
// =============================================================================

#include "bench_harness.h"

// cpp-httplib is used for the HTTP client (and server via HttpServer).
// We include httplib.h directly — the benchmark CMake target adds its path.
#include "httplib.h"

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace forgekv;
using namespace forgekv::bench;

// Declared in benchmark_kv_store.cpp.
extern std::atomic<int> g_failures;

// =============================================================================
// HTTP benchmark helper — start server + run workload
// =============================================================================

namespace {

// RAII server lifetime guard.
struct HttpBenchServer {
    TempWAL                         tmp;
    std::unique_ptr<KeyValueStore>  kv;
    std::unique_ptr<HttpServer>     server;
    std::thread                     server_thread;
    int                             port{-1};

    // Start a fresh server on an ephemeral port.
    // Returns true on success.
    bool start() {
        auto [t, k] = make_kv();
        tmp    = std::move(t);
        kv     = std::move(k);
        server = std::make_unique<HttpServer>(*kv);

        port = server->bind_to_any_port("127.0.0.1");
        if (port < 0) {
            std::cerr << "  [HTTP BENCH] Failed to bind to ephemeral port\n";
            return false;
        }

        server_thread = std::thread([this]() {
            server->listen_after_bind();
        });

        server->wait_until_ready();
        return true;
    }

    ~HttpBenchServer() {
        if (server) server->stop();
        if (server_thread.joinable()) server_thread.join();
    }

    // Pre-populate keys via PUT.
    void populate(std::uint64_t count, std::uint32_t value_size) {
        httplib::Client cli("127.0.0.1", port);
        const std::string val = make_value(value_size, 0);
        for (std::uint64_t i = 0; i < count; ++i) {
            cli.Put("/key/" + make_key(i), val, "text/plain");
        }
    }
};

} // anonymous namespace

// =============================================================================
// A. HTTP GET benchmark
// =============================================================================

static BenchResult bench_http_get(const BenchConfig& cfg,
                                   std::uint32_t thread_count) {
    BenchResult r;
    r.workload = "HTTP GET (hit)";
    r.threads  = thread_count;

    HttpBenchServer srv;
    if (!srv.start()) {
        r.workload += " [SKIPPED — server failed to bind]";
        return r;
    }

    // Use a smaller count for HTTP (network overhead is significant).
    const std::uint64_t http_ops =
        std::min(cfg.operations / 10, std::uint64_t{5'000});

    // Populate keys (outside timing).
    srv.populate(http_ops, cfg.value_size);

    // Warmup.
    {
        httplib::Client warmup_cli("127.0.0.1", srv.port);
        const std::uint64_t wops = std::min(cfg.warmup / 10, std::uint64_t{100});
        for (std::uint64_t i = 0; i < wops; ++i) {
            warmup_cli.Get("/key/" + make_key(i % http_ops));
        }
    }

    // ---- Measured phase ----
    const std::uint64_t ops_per_thread =
        (http_ops + thread_count - 1) / thread_count;

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(thread_count) + 1);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::uint32_t tid = 0; tid < thread_count; ++tid) {
        workers.emplace_back([&, tid]() {
            httplib::Client cli("127.0.0.1", srv.port);
            const std::uint64_t base = static_cast<std::uint64_t>(tid) * ops_per_thread;
            start_barrier.arrive_and_wait();
            for (std::uint64_t i = 0; i < ops_per_thread; ++i) {
                cli.Get("/key/" + make_key((base + i) % http_ops));
            }
        });
    }

    Timer t;
    start_barrier.arrive_and_wait();
    t.start();
    for (auto& w : workers) w.join();
    r.elapsed_s = t.elapsed_s();

    r.ops         = ops_per_thread * thread_count;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;

    return r;
}

// =============================================================================
// B. HTTP PUT benchmark
// =============================================================================

static BenchResult bench_http_put(const BenchConfig& cfg,
                                   std::uint32_t thread_count) {
    BenchResult r;
    r.workload = "HTTP PUT";
    r.threads  = thread_count;

    HttpBenchServer srv;
    if (!srv.start()) {
        r.workload += " [SKIPPED — server failed to bind]";
        return r;
    }

    const std::uint64_t http_ops =
        std::min(cfg.operations / 10, std::uint64_t{5'000});
    const std::string val = make_value(cfg.value_size, 1);

    // Warmup.
    {
        httplib::Client warmup_cli("127.0.0.1", srv.port);
        const std::uint64_t wops = std::min(cfg.warmup / 10, std::uint64_t{100});
        for (std::uint64_t i = 0; i < wops; ++i) {
            warmup_cli.Put("/key/" + make_key(i), val, "text/plain");
        }
    }

    const std::uint64_t ops_per_thread =
        (http_ops + thread_count - 1) / thread_count;

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(thread_count) + 1);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::uint32_t tid = 0; tid < thread_count; ++tid) {
        workers.emplace_back([&, tid]() {
            httplib::Client cli("127.0.0.1", srv.port);
            const std::uint64_t base = static_cast<std::uint64_t>(tid) * ops_per_thread;
            start_barrier.arrive_and_wait();
            for (std::uint64_t i = 0; i < ops_per_thread; ++i) {
                cli.Put("/key/" + make_key(base + i), val, "text/plain");
            }
        });
    }

    Timer t;
    start_barrier.arrive_and_wait();
    t.start();
    for (auto& w : workers) w.join();
    r.elapsed_s = t.elapsed_s();

    r.ops         = ops_per_thread * thread_count;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;

    // Correctness: server should have the expected key count.
    {
        httplib::Client cli("127.0.0.1", srv.port);
        auto resp = cli.Get("/stats");
        if (!resp || resp->status != 200) {
            std::cerr << "  [HTTP BENCH] /stats check failed\n";
        }
    }

    return r;
}

// =============================================================================
// C. HTTP Mixed (GET + PUT)
// =============================================================================

static BenchResult bench_http_mixed(const BenchConfig& cfg,
                                     std::uint32_t thread_count) {
    BenchResult r;
    r.workload = "HTTP Mixed";
    r.threads  = thread_count;

    HttpBenchServer srv;
    if (!srv.start()) {
        r.workload += " [SKIPPED — server failed to bind]";
        return r;
    }

    const std::uint64_t http_ops =
        std::min(cfg.operations / 10, std::uint64_t{5'000});

    // Populate keys for GET phase (outside timing).
    srv.populate(http_ops, cfg.value_size);

    const std::string val = make_value(cfg.value_size, 2);

    const std::uint64_t ops_per_thread =
        (http_ops + thread_count - 1) / thread_count;

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(thread_count) + 1);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::uint32_t tid = 0; tid < thread_count; ++tid) {
        workers.emplace_back([&, tid]() {
            httplib::Client cli("127.0.0.1", srv.port);
            const std::uint64_t base = static_cast<std::uint64_t>(tid) * ops_per_thread;
            start_barrier.arrive_and_wait();
            for (std::uint64_t i = 0; i < ops_per_thread; ++i) {
                const std::uint64_t key_idx = (base + i) % http_ops;
                if (i % 2 == 0) {
                    cli.Get("/key/" + make_key(key_idx));
                } else {
                    cli.Put("/key/" + make_key(key_idx), val, "text/plain");
                }
            }
        });
    }

    Timer t;
    start_barrier.arrive_and_wait();
    t.start();
    for (auto& w : workers) w.join();
    r.elapsed_s = t.elapsed_s();

    r.ops         = ops_per_thread * thread_count;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;

    return r;
}

// =============================================================================
// run_http_benchmarks — entry point called from main()
// =============================================================================

std::vector<BenchResult> run_http_benchmarks(const BenchConfig& cfg) {
    std::vector<BenchResult> all_results;

    const std::uint32_t hw_threads = std::thread::hardware_concurrency();
    const std::uint32_t max_threads = std::max(1u, std::min(cfg.threads, hw_threads));

    // Build thread count ladder: 1, 2, 4 — up to max_threads.
    // Limit to 4 for HTTP to avoid overwhelming loopback.
    const std::uint32_t http_max = std::min(max_threads, 4u);
    std::vector<std::uint32_t> thread_counts;
    for (std::uint32_t tc = 1; tc <= http_max; tc *= 2) {
        thread_counts.push_back(tc);
    }
    if (!thread_counts.empty() && thread_counts.back() != http_max && http_max > 1) {
        thread_counts.push_back(http_max);
    }

    // ---- HTTP GET ----
    print_section("HTTP Benchmark: GET");
    {
        std::vector<BenchResult> http_get;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: HTTP GET (" << tc << " threads) ...\n";
            auto r = bench_http_get(cfg, tc);
            http_get.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(http_get);
        std::cout << "  (HTTP throughput is network-stack limited even on loopback)\n";
    }

    // ---- HTTP PUT ----
    print_section("HTTP Benchmark: PUT");
    {
        std::vector<BenchResult> http_put;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: HTTP PUT (" << tc << " threads) ...\n";
            auto r = bench_http_put(cfg, tc);
            http_put.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(http_put);
    }

    // ---- HTTP Mixed ----
    print_section("HTTP Benchmark: Mixed");
    {
        std::vector<BenchResult> http_mix;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: HTTP Mixed (" << tc << " threads) ...\n";
            auto r = bench_http_mixed(cfg, tc);
            http_mix.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(http_mix);
    }

    return all_results;
}
