// =============================================================================
// ForgeKV — Stage 12: Benchmark Entry Point
// =============================================================================
//
// Parses command-line arguments, prints environment info, runs all benchmarks,
// prints the final summary, and optionally writes results to CSV.
//
// Usage:
//   ./forgekv_benchmark [options]
//   ./forgekv_benchmark --help
// =============================================================================

#include "bench_harness.h"

#include <atomic>
#include <iostream>
#include <vector>

using namespace forgekv::bench;

// Declared in benchmark_kv_store.cpp and benchmark_http.cpp.
extern std::vector<BenchResult> run_kv_benchmarks(const BenchConfig& cfg);
extern std::vector<BenchResult> run_http_benchmarks(const BenchConfig& cfg);

// Declared in benchmark_kv_store.cpp.
extern std::atomic<int> g_failures;

int main(int argc, char* argv[]) {
    const BenchConfig cfg = parse_args(argc, argv);

    print_header("ForgeKV Benchmark — Stage 12");
    print_env_info();
    print_config(cfg);

    std::vector<BenchResult> all_results;

    // ---- KV-level benchmarks ----
    auto kv_results = run_kv_benchmarks(cfg);
    for (auto& r : kv_results) all_results.push_back(r);

    // ---- HTTP benchmarks ----
    if (cfg.run_http) {
        auto http_results = run_http_benchmarks(cfg);
        for (auto& r : http_results) all_results.push_back(r);
    } else {
        std::cout << "\n[HTTP benchmark skipped (--no-http)]\n";
    }

    // ---- Final summary ----
    print_section("Final Summary");
    std::cout << "  Total workloads measured : " << all_results.size() << '\n';
    const int failures = g_failures.load(std::memory_order_relaxed);
    if (failures == 0) {
        std::cout << "  Correctness checks       : ALL PASSED\n";
    } else {
        std::cout << "  Correctness checks       : " << failures << " FAILED\n";
    }

    // ---- CSV output ----
    if (!cfg.output_file.empty()) {
        write_csv(cfg.output_file, all_results);
    }

    print_separator('=');
    std::cout << '\n';

    return (failures > 0) ? 1 : 0;
}
