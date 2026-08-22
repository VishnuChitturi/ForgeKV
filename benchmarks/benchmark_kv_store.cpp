// =============================================================================
// ForgeKV — Stage 12: KV Store Benchmark
// =============================================================================
//
// Measures throughput, latency, and concurrency scalability of KeyValueStore.
//
// Workloads:
//   A. Sequential SET
//   B. Sequential GET HIT
//   C. Sequential GET MISS
//   D. Sequential DELETE
//   E. Mixed workload (50% GET / 30% SET / 10% DEL / 10% GET MISS)
//   F. TTL workload (set_with_ttl)
//   G. Snapshot workload
//   H. Compaction workload
//   I. Concurrency — concurrent GET / SET / mixed
//   J. Latency sampling — SET and GET
//
// Timing methodology:
//   Each workload creates its own isolated KeyValueStore via make_kv().
//   Warmup runs first (not measured). Timer starts AFTER warmup is complete.
//   Timer stops BEFORE correctness verification and stats collection.
//   Setup (filling the store before a GET/DELETE bench) is outside timing.
//
// Correctness verification:
//   After each workload, expected state is verified against stats().
//   A failure prints an error and increments the global failure counter.
//   Verification happens OUTSIDE measured timing.
//
// WAL behavior:
//   All SET/DELETE/TTL operations exercise the real WAL.
//   WAL size is reported before and after each workload.
//   Compaction and snapshot workloads explicitly test WAL reduction.
// =============================================================================

#include "bench_harness.h"

#include "forgekv/kv_store.h"
#include "forgekv/stats.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <atomic>
#include <barrier>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace forgekv;
using namespace forgekv::bench;

// Global failure counter — incremented on correctness check failure.
// Non-static: extern-referenced from benchmark_http.cpp and benchmark_main.cpp.
std::atomic<int> g_failures{0};

// Helper: check a condition and report failure outside timing.
static void verify(bool condition, const std::string& label,
                   const std::string& detail = "") {
    if (!condition) {
        std::cerr << "  [FAIL] Correctness check failed: " << label;
        if (!detail.empty()) std::cerr << " — " << detail;
        std::cerr << '\n';
        g_failures.fetch_add(1, std::memory_order_relaxed);
    }
}

// =============================================================================
// A. Sequential SET
// =============================================================================

static BenchResult bench_sequential_set(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 0);
    BenchResult r;
    r.workload = "Sequential SET";
    r.threads  = 1;

    // Warmup (not measured).
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
        kv->set(make_key(i), val);
    }
    // Reset: recreate store so warmup keys don't inflate WAL.
    kv.reset();
    tmp.cleanup();
    auto [tmp2, kv2] = make_kv();

    r.wal_before = kv2->stats().wal_size_bytes;

    Timer t;
    t.start();
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        kv2->set(make_key(i), val);
    }
    r.elapsed_s = t.elapsed_s();
    r.ops       = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv2->stats().wal_size_bytes;

    // Correctness: every key must be present.
    const auto s = kv2->stats();
    verify(s.key_count == cfg.operations, "SET: key_count",
           std::to_string(s.key_count) + " != " + std::to_string(cfg.operations));
    verify(s.set_count == cfg.operations, "SET: set_count");

    return r;
}

// =============================================================================
// B. Sequential GET HIT
// =============================================================================

static BenchResult bench_sequential_get_hit(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 1);
    BenchResult r;
    r.workload = "Sequential GET (hit)";
    r.threads  = 1;

    // Setup: populate keys (outside timing).
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        kv->set(make_key(i), val);
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    std::uint64_t hit_count = 0;
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        auto v = kv->get(make_key(i));
        if (v.has_value()) ++hit_count;
    }
    r.elapsed_s   = t.elapsed_s();
    r.ops         = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness: every GET must be a hit.
    verify(hit_count == cfg.operations, "GET HIT: hit_count",
           std::to_string(hit_count) + " != " + std::to_string(cfg.operations));
    const auto s = kv->stats();
    verify(s.get_hits >= cfg.operations, "GET HIT: stats.get_hits");

    return r;
}

// =============================================================================
// C. Sequential GET MISS
// =============================================================================

static BenchResult bench_sequential_get_miss(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    BenchResult r;
    r.workload = "Sequential GET (miss)";
    r.threads  = 1;

    // The store is empty — every GET should miss.
    // Warmup using miss-only gets is harmless (no state change).
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
        (void)kv->get(make_key(i));
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    std::uint64_t miss_count = 0;
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        auto v = kv->get(make_key(i));
        if (!v.has_value()) ++miss_count;
    }
    r.elapsed_s   = t.elapsed_s();
    r.ops         = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness: every GET must be a miss.
    verify(miss_count == cfg.operations, "GET MISS: miss_count",
           std::to_string(miss_count) + " != " + std::to_string(cfg.operations));

    return r;
}

// =============================================================================
// D. Sequential DELETE
// =============================================================================

static BenchResult bench_sequential_delete(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 2);
    BenchResult r;
    r.workload = "Sequential DELETE";
    r.threads  = 1;

    // Setup: populate keys (outside timing).
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        kv->set(make_key(i), val);
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        kv->del(make_key(i));
    }
    r.elapsed_s   = t.elapsed_s();
    r.ops         = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness: all keys should be gone.
    const auto s = kv->stats();
    verify(s.key_count == 0, "DELETE: key_count after deletion",
           "key_count=" + std::to_string(s.key_count));
    verify(s.delete_count == cfg.operations, "DELETE: delete_count");

    return r;
}

// =============================================================================
// E. Mixed workload (50% GET / 30% SET / 10% DEL / 10% GET MISS)
// =============================================================================
//
// Keyspace: operations keys, half pre-populated so GETs mostly hit.
// Distribution per operation index i:
//   i % 10 == 0,1,2,3,4  → GET  (key i % ops)        [50% — some hit, some miss]
//   i % 10 == 5,6,7       → SET  (key i % ops)        [30%]
//   i % 10 == 8           → DEL  (key i % ops)        [10%]
//   i % 10 == 9           → GET miss (key ops + i)    [10% — key guaranteed absent]

static BenchResult bench_mixed(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 3);
    BenchResult r;
    r.workload = "Mixed (50G/30S/10D/10M)";
    r.threads  = 1;

    // Setup: pre-populate half the keyspace (outside timing).
    for (std::uint64_t i = 0; i < cfg.operations / 2; ++i) {
        kv->set(make_key(i), val);
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        const std::uint64_t op = i % 10;
        if (op < 5) {
            (void)kv->get(make_key(i % cfg.operations));
        } else if (op < 8) {
            kv->set(make_key(i % cfg.operations), val);
        } else if (op == 8) {
            kv->del(make_key(i % cfg.operations));
        } else {
            // Guaranteed miss: key index beyond keyspace.
            (void)kv->get(make_key(cfg.operations + i));
        }
    }
    r.elapsed_s   = t.elapsed_s();
    r.ops         = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness: stats counters must be consistent.
    const auto s = kv->stats();
    verify(s.get_hits + s.get_misses > 0, "MIXED: get ops recorded");
    verify(s.set_count > 0, "MIXED: set ops recorded");
    // Spot-check: a key known to have been SET on the last SET operation
    // should exist (or may have been subsequently deleted — just verify the
    // overall stats are non-zero and WAL grew).
    verify(r.wal_after > 0, "MIXED: WAL is non-empty after workload");

    return r;
}

// =============================================================================
// F. TTL workload
// =============================================================================

static BenchResult bench_ttl(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 4);
    BenchResult r;
    r.workload = "TTL SET (set_with_ttl)";
    r.threads  = 1;

    // Use a TTL long enough that no expiration occurs during the benchmark.
    const double ttl_seconds = 3600.0; // 1 hour

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    for (std::uint64_t i = 0; i < cfg.operations; ++i) {
        kv->set_with_ttl(make_key(i), val, ttl_seconds);
    }
    r.elapsed_s   = t.elapsed_s();
    r.ops         = cfg.operations;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness.
    const auto s = kv->stats();
    verify(s.key_count == cfg.operations, "TTL SET: key_count",
           std::to_string(s.key_count) + " != " + std::to_string(cfg.operations));
    verify(s.ttl_set_count == cfg.operations, "TTL SET: ttl_set_count");
    // Verify a sample key has a TTL > 0.
    const double remaining = kv->ttl(make_key(0));
    verify(remaining > 0.0, "TTL SET: sample key has positive TTL",
           "ttl=" + std::to_string(remaining));

    return r;
}

// =============================================================================
// G. Snapshot workload
// =============================================================================
//
// Measures the time to call snapshot() on a meaningful dataset.
// Reports: duration, key count, WAL size before/after snapshot.

static BenchResult bench_snapshot(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 5);
    BenchResult r;
    r.workload = "Snapshot";
    r.threads  = 1;

    // Populate dataset (outside timing).
    const std::uint64_t snap_keys = std::min(cfg.operations, std::uint64_t{10'000});
    for (std::uint64_t i = 0; i < snap_keys; ++i) {
        kv->set(make_key(i), val);
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    const bool ok = kv->snapshot();
    r.elapsed_s   = t.elapsed_s();
    r.ops         = snap_keys;   // report dataset size as "ops"
    r.ops_per_sec = 0.0;         // not a throughput benchmark
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness.
    verify(ok, "SNAPSHOT: snapshot() returned true");
    const auto s = kv->stats();
    verify(s.key_count == snap_keys, "SNAPSHOT: key count preserved after snapshot",
           std::to_string(s.key_count) + " != " + std::to_string(snap_keys));
    verify(s.last_snapshot_time_us > 0, "SNAPSHOT: last_snapshot_time_us updated");

    // Print snapshot-specific details.
    std::cout << "    Dataset: " << snap_keys << " keys × "
              << cfg.value_size << " bytes\n"
              << "    Duration: " << std::fixed << std::setprecision(6)
              << r.elapsed_s << " s\n"
              << "    WAL before: " << r.wal_before << " bytes\n"
              << "    WAL at snapshot: " << r.wal_after << " bytes\n";
    // Snapshot file size.
    const std::string snap_path = tmp.path() + ".snapshot";
    if (std::filesystem::exists(snap_path)) {
        const auto snap_sz = std::filesystem::file_size(snap_path);
        std::cout << "    Snapshot file: " << snap_sz << " bytes\n";
    }

    return r;
}

// =============================================================================
// H. Compaction workload
// =============================================================================
//
// Creates repeated updates so the WAL is bloated with redundant records,
// then measures compact() duration and reduction ratio.

static BenchResult bench_compaction(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 6);
    BenchResult r;
    r.workload = "Compaction";
    r.threads  = 1;

    // Setup: write each key multiple times to bloat the WAL (outside timing).
    const std::uint64_t compact_keys     = std::min(cfg.operations / 10, std::uint64_t{5'000});
    const std::uint64_t rewrites_per_key = 5;
    for (std::uint64_t rewrite = 0; rewrite < rewrites_per_key; ++rewrite) {
        for (std::uint64_t i = 0; i < compact_keys; ++i) {
            kv->set(make_key(i),
                    make_value(cfg.value_size, rewrite * compact_keys + i));
        }
    }

    r.wal_before = kv->stats().wal_size_bytes;

    Timer t;
    t.start();
    kv->compact();
    r.elapsed_s   = t.elapsed_s();
    r.ops         = compact_keys;  // report key count as "ops"
    r.ops_per_sec = 0.0;           // not throughput
    r.wal_after   = kv->stats().wal_size_bytes;

    const double reduction = (r.wal_before > 0)
        ? (1.0 - static_cast<double>(r.wal_after) / static_cast<double>(r.wal_before)) * 100.0
        : 0.0;

    // Correctness: all live keys must still be accessible.
    const auto s = kv->stats();
    verify(s.key_count == compact_keys, "COMPACT: key_count after compaction",
           std::to_string(s.key_count) + " != " + std::to_string(compact_keys));
    // Spot-check a key.
    for (std::uint64_t i = 0; i < std::min(compact_keys, std::uint64_t{5}); ++i) {
        const auto v = kv->get(make_key(i));
        verify(v.has_value(), "COMPACT: spot-check key still exists",
               "key=" + make_key(i));
    }
    // WAL should be smaller after compaction.
    verify(r.wal_after < r.wal_before, "COMPACT: WAL size reduced",
           "before=" + std::to_string(r.wal_before)
           + " after=" + std::to_string(r.wal_after));

    std::cout << "    Keys: " << compact_keys
              << " × " << rewrites_per_key << " rewrites\n"
              << "    WAL before: " << r.wal_before << " bytes\n"
              << "    WAL after:  " << r.wal_after  << " bytes\n"
              << "    Reduction:  " << std::fixed << std::setprecision(1)
              << reduction << "%\n"
              << "    Duration:   " << std::fixed << std::setprecision(6)
              << r.elapsed_s << " s\n";

    return r;
}

// =============================================================================
// I. Concurrency benchmarks
// =============================================================================
//
// Workers synchronize at a std::barrier so all threads start the measured
// phase together. Total elapsed time is measured from the common start point.
// Thread counts: 1, 2, 4, 8 — capped at hardware_concurrency.

enum class ConcurrencyMode { Get, Set, Mixed };

static BenchResult bench_concurrency(const BenchConfig& cfg,
                                     std::uint32_t thread_count,
                                     ConcurrencyMode mode) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 7);
    BenchResult r;
    r.threads = thread_count;
    switch (mode) {
        case ConcurrencyMode::Get:   r.workload = "Concurrent GET";   break;
        case ConcurrencyMode::Set:   r.workload = "Concurrent SET";   break;
        case ConcurrencyMode::Mixed: r.workload = "Concurrent Mixed"; break;
    }

    // Setup: pre-populate keys for GET workloads (outside timing).
    if (mode == ConcurrencyMode::Get || mode == ConcurrencyMode::Mixed) {
        for (std::uint64_t i = 0; i < cfg.operations; ++i) {
            kv->set(make_key(i), val);
        }
    }

    // Each thread processes ops_per_thread operations.
    const std::uint64_t ops_per_thread =
        (cfg.operations + thread_count - 1) / thread_count;

    r.wal_before = kv->stats().wal_size_bytes;

    // Barrier: all workers + timing thread (main) synchronize.
    // n_parties = thread_count + 1 (worker threads + timer thread).
    // The barrier is used twice: arrival (ready) and departure (done).
    std::atomic<bool> go{false};
    std::atomic<bool> done_flag{false};

    // Use std::barrier for simultaneous launch.
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(thread_count) + 1);

    std::atomic<double> total_elapsed_s{0.0};
    Timer global_timer;

    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    for (std::uint32_t tid = 0; tid < thread_count; ++tid) {
        workers.emplace_back([&, tid]() {
            const std::uint64_t base   = static_cast<std::uint64_t>(tid) * ops_per_thread;
            const std::uint64_t my_val_seed = static_cast<std::uint64_t>(tid);
            const std::string   my_val = make_value(cfg.value_size, my_val_seed);

            // Signal ready; wait for all workers + timer to arrive.
            start_barrier.arrive_and_wait();

            // --- Measured section ---
            for (std::uint64_t i = 0; i < ops_per_thread; ++i) {
                const std::uint64_t key_idx = (base + i) % cfg.operations;
                switch (mode) {
                    case ConcurrencyMode::Get:
                        (void)kv->get(make_key(key_idx));
                        break;
                    case ConcurrencyMode::Set:
                        kv->set(make_key(key_idx), my_val);
                        break;
                    case ConcurrencyMode::Mixed:
                        if (i % 2 == 0) {
                            (void)kv->get(make_key(key_idx));
                        } else {
                            kv->set(make_key(key_idx), my_val);
                        }
                        break;
                }
            }
            // --- End measured section ---
        });
    }

    // Timer thread: arrive at barrier with workers, start clock, wait for all.
    start_barrier.arrive_and_wait();
    global_timer.start();

    for (auto& w : workers) w.join();
    r.elapsed_s = global_timer.elapsed_s();

    r.ops         = ops_per_thread * thread_count;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;
    r.wal_after   = kv->stats().wal_size_bytes;

    // Correctness: store still responds after concurrent access.
    if (mode == ConcurrencyMode::Set || mode == ConcurrencyMode::Mixed) {
        // At least one key from the workload must exist.
        bool found_any = false;
        for (std::uint64_t i = 0; i < std::min(cfg.operations, std::uint64_t{10}); ++i) {
            if (kv->get(make_key(i)).has_value()) { found_any = true; break; }
        }
        verify(found_any, r.workload + ": at least one key survives concurrent writes");
    }
    if (mode == ConcurrencyMode::Get) {
        verify(kv->stats().get_hits > 0, r.workload + ": get_hits > 0 after concurrent reads");
    }

    return r;
}

// =============================================================================
// J. Latency sampling — SET and GET
// =============================================================================
//
// Runs a single-threaded workload where EVERY operation is individually timed.
// The latency sample set is sorted and percentiles computed.
// The throughput number from a latency benchmark is NOT representative —
// the per-op timing overhead adds real cost.

static BenchResult bench_latency_set(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 8);
    BenchResult r;
    r.workload = "Latency SET";
    r.threads  = 1;

    const std::uint64_t lat_ops = std::min(cfg.operations, cfg.latency_samples);

    // Warmup (not measured, not timed per-op).
    for (std::uint64_t i = 0; i < cfg.warmup; ++i) {
        kv->set(make_key(i), val);
    }

    std::vector<double> samples;
    samples.reserve(lat_ops);

    Timer total;
    total.start();

    for (std::uint64_t i = 0; i < lat_ops; ++i) {
        Timer per_op;
        per_op.start();
        kv->set(make_key(cfg.warmup + i), val);
        samples.push_back(per_op.elapsed_us());
    }

    r.elapsed_s   = total.elapsed_s();
    r.ops         = lat_ops;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;

    const auto stats = compute_latency(samples);
    r.lat_avg_us = stats.avg_us;
    r.lat_p50_us = stats.p50_us;
    r.lat_p95_us = stats.p95_us;
    r.lat_p99_us = stats.p99_us;

    return r;
}

static BenchResult bench_latency_get(const BenchConfig& cfg) {
    auto [tmp, kv] = make_kv();
    const std::string val = make_value(cfg.value_size, 9);
    BenchResult r;
    r.workload = "Latency GET";
    r.threads  = 1;

    const std::uint64_t lat_ops = std::min(cfg.operations, cfg.latency_samples);

    // Populate keys (outside timing).
    for (std::uint64_t i = 0; i < lat_ops; ++i) {
        kv->set(make_key(i), val);
    }

    std::vector<double> samples;
    samples.reserve(lat_ops);

    Timer total;
    total.start();

    for (std::uint64_t i = 0; i < lat_ops; ++i) {
        Timer per_op;
        per_op.start();
        (void)kv->get(make_key(i));
        samples.push_back(per_op.elapsed_us());
    }

    r.elapsed_s   = total.elapsed_s();
    r.ops         = lat_ops;
    r.ops_per_sec = static_cast<double>(r.ops) / r.elapsed_s;

    const auto stats = compute_latency(samples);
    r.lat_avg_us = stats.avg_us;
    r.lat_p50_us = stats.p50_us;
    r.lat_p95_us = stats.p95_us;
    r.lat_p99_us = stats.p99_us;

    return r;
}

// =============================================================================
// run_kv_benchmarks — entry point called from main()
// =============================================================================

std::vector<BenchResult> run_kv_benchmarks(const BenchConfig& cfg) {
    std::vector<BenchResult> all_results;

    // -------------------------------------------------------------------------
    // Sequential throughput workloads
    // -------------------------------------------------------------------------
    print_section("Sequential Throughput Benchmarks");

    {
        std::cout << "  Running: Sequential SET ...\n";
        auto r = bench_sequential_set(cfg);
        all_results.push_back(r);
    }
    {
        std::cout << "  Running: Sequential GET HIT ...\n";
        auto r = bench_sequential_get_hit(cfg);
        all_results.push_back(r);
    }
    {
        std::cout << "  Running: Sequential GET MISS ...\n";
        auto r = bench_sequential_get_miss(cfg);
        all_results.push_back(r);
    }
    {
        std::cout << "  Running: Sequential DELETE ...\n";
        auto r = bench_sequential_delete(cfg);
        all_results.push_back(r);
    }
    {
        std::cout << "  Running: Mixed Workload ...\n";
        auto r = bench_mixed(cfg);
        all_results.push_back(r);
    }
    {
        std::cout << "  Running: TTL SET ...\n";
        auto r = bench_ttl(cfg);
        all_results.push_back(r);
    }

    // Print throughput table.
    std::vector<BenchResult> seq_results(all_results.begin(), all_results.end());
    print_section("Throughput Results");
    print_throughput_table(seq_results);

    // -------------------------------------------------------------------------
    // Snapshot & Compaction (reported separately — not throughput)
    // -------------------------------------------------------------------------
    print_section("Snapshot Benchmark");
    {
        std::cout << "  Running: Snapshot ...\n";
        auto r = bench_snapshot(cfg);
        all_results.push_back(r);
    }

    print_section("Compaction Benchmark");
    {
        std::cout << "  Running: Compaction ...\n";
        auto r = bench_compaction(cfg);
        all_results.push_back(r);
    }

    // -------------------------------------------------------------------------
    // Concurrency benchmarks
    // -------------------------------------------------------------------------
    const std::uint32_t hw_threads = std::thread::hardware_concurrency();
    const std::uint32_t max_threads = std::max(1u, std::min(cfg.threads, hw_threads));

    // Build thread count ladder: 1, 2, 4, 8 — up to max_threads.
    std::vector<std::uint32_t> thread_counts;
    for (std::uint32_t tc = 1; tc <= max_threads; tc *= 2) {
        thread_counts.push_back(tc);
    }
    // Always include max_threads if not already a power of 2.
    if (thread_counts.back() != max_threads && max_threads > 1) {
        thread_counts.push_back(max_threads);
    }

    // Concurrent GET
    print_section("Concurrency: GET");
    {
        std::vector<BenchResult> conc_get;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: Concurrent GET (" << tc << " threads) ...\n";
            auto r = bench_concurrency(cfg, tc, ConcurrencyMode::Get);
            conc_get.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(conc_get);
    }

    // Concurrent SET
    print_section("Concurrency: SET");
    {
        std::vector<BenchResult> conc_set;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: Concurrent SET (" << tc << " threads) ...\n";
            auto r = bench_concurrency(cfg, tc, ConcurrencyMode::Set);
            conc_set.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(conc_set);
    }

    // Concurrent Mixed
    print_section("Concurrency: Mixed");
    {
        std::vector<BenchResult> conc_mix;
        for (std::uint32_t tc : thread_counts) {
            std::cout << "  Running: Concurrent Mixed (" << tc << " threads) ...\n";
            auto r = bench_concurrency(cfg, tc, ConcurrencyMode::Mixed);
            conc_mix.push_back(r);
            all_results.push_back(r);
        }
        print_concurrency_table(conc_mix);
    }

    // -------------------------------------------------------------------------
    // Latency benchmarks
    // -------------------------------------------------------------------------
    if (cfg.latency) {
        print_section("Latency Benchmarks");
        std::cout << "  (Per-operation timing overhead is included in latency numbers)\n";
        {
            std::cout << "  Running: Latency SET ...\n";
            auto r = bench_latency_set(cfg);
            all_results.push_back(r);
        }
        {
            std::cout << "  Running: Latency GET ...\n";
            auto r = bench_latency_get(cfg);
            all_results.push_back(r);
        }
        print_section("Latency Results");
        std::vector<BenchResult> lat_results;
        for (const auto& r : all_results) {
            if (r.lat_p50_us > 0.0) lat_results.push_back(r);
        }
        print_latency_table(lat_results);
    }

    // -------------------------------------------------------------------------
    // WAL summary
    // -------------------------------------------------------------------------
    {
        bool has_wal_data = false;
        for (const auto& r : all_results) {
            if (r.wal_before > 0 || r.wal_after > 0) { has_wal_data = true; break; }
        }
        if (has_wal_data) {
            print_section("WAL Size");
            print_wal_table(all_results);
        }
    }

    return all_results;
}
