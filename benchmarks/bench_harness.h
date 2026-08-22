#pragma once
// =============================================================================
// ForgeKV — Stage 12: Benchmark Harness
// =============================================================================
//
// Lightweight, self-contained benchmark harness. No external dependencies.
//
// Features:
//   - BenchConfig   — runtime-configurable benchmark parameters
//   - BenchResult   — per-workload result record
//   - Timer         — std::chrono::steady_clock-based elapsed timer
//   - TempWAL       — RAII temporary WAL file for isolated benchmarks
//   - make_kv       — factory: creates a KeyValueStore over a fresh temp WAL
//   - make_key/val  — deterministic key/value generators (fixed seed)
//   - latency_stats — compute p50/p95/p99 from a sorted sample vector
//   - print_*       — formatted output helpers
//
// Timing methodology:
//   Every measured section is wrapped in Timer::start() / Timer::elapsed_s().
//   Warmup, setup, teardown, and correctness checks are always OUTSIDE the
//   measured window. std::chrono::steady_clock guarantees monotonicity.
//
// WAL isolation:
//   Each benchmark creates a KeyValueStore via make_kv(), which points to a
//   unique temp WAL file in /tmp. The WAL file is deleted on scope exit by
//   TempWAL's destructor. This prevents one benchmark's persistent state from
//   polluting another's.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace forgekv::bench {

// =============================================================================
// BenchConfig — benchmark parameters (parsed from CLI or defaulted)
// =============================================================================

struct BenchConfig {
    std::uint64_t operations  = 100'000;   // default operation count
    std::uint64_t warmup      = 1'000;     // warmup operations (not measured)
    std::uint32_t value_size  = 128;       // value payload size in bytes
    std::uint32_t threads     = 4;         // thread count for concurrency bench
    bool          latency     = true;      // collect latency samples
    std::uint64_t latency_samples = 10'000; // max latency samples to collect
    std::string   output_file;             // empty = no CSV output
    std::string   json_output_file;        // empty = no JSON output; "-" = stdout
    bool          run_http    = true;      // run HTTP benchmark
    bool          run_large   = false;     // run large dataset (1M ops)
};

// =============================================================================
// BenchResult — one workload's measured outcome
// =============================================================================

struct BenchResult {
    std::string   workload;           // human-readable name
    std::uint64_t ops         = 0;    // operations performed (measured phase)
    double        elapsed_s   = 0.0;  // measured wall time (seconds)
    double        ops_per_sec = 0.0;  // throughput
    std::uint64_t wal_before  = 0;    // WAL size before workload
    std::uint64_t wal_after   = 0;    // WAL size after workload
    std::uint32_t threads     = 1;    // thread count (concurrency benchmarks)

    // Latency (in microseconds). 0 if not measured.
    double lat_avg_us = 0.0;
    double lat_p50_us = 0.0;
    double lat_p95_us = 0.0;
    double lat_p99_us = 0.0;
};

// =============================================================================
// Timer — steady_clock-based elapsed timer
// =============================================================================

class Timer {
public:
    // Start (or restart) the timer.
    void start() noexcept {
        start_ = std::chrono::steady_clock::now();
    }

    // Return elapsed seconds since start(). Does not stop the timer.
    [[nodiscard]] double elapsed_s() const noexcept {
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(end - start_).count();
    }

    // Return elapsed microseconds since start().
    [[nodiscard]] double elapsed_us() const noexcept {
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }

private:
    std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
};

// =============================================================================
// TempWAL — RAII guard for a unique temporary WAL file
// =============================================================================

class TempWAL {
public:
    // Create a unique temporary WAL path in /tmp.
    TempWAL() {
        // Use thread ID + counter for uniqueness even under concurrent use.
        static std::atomic<std::uint64_t> counter{0};
        const auto id = counter.fetch_add(1, std::memory_order_relaxed);
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        path_ = std::string("/tmp/forgekv_bench_") + std::to_string(tid)
                + "_" + std::to_string(id) + ".wal";
    }

    ~TempWAL() {
        cleanup();
    }

    // Not copyable.
    TempWAL(const TempWAL&)            = delete;
    TempWAL& operator=(const TempWAL&) = delete;

    // Movable.
    TempWAL(TempWAL&& other) noexcept : path_(std::move(other.path_)) {
        other.path_.clear();
    }
    TempWAL& operator=(TempWAL&& other) noexcept {
        if (this != &other) {
            cleanup();
            path_ = std::move(other.path_);
            other.path_.clear();
        }
        return *this;
    }

    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    void cleanup() noexcept {
        if (!path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
            // Also remove snapshot file if it exists.
            std::filesystem::remove(path_ + ".snapshot", ec);
            path_.clear();
        }
    }

private:
    std::string path_;
};

// =============================================================================
// make_kv — factory: isolated KeyValueStore backed by a fresh TempWAL
// =============================================================================
//
// Returns a pair: (TempWAL, KeyValueStore).
// The TempWAL must be kept alive for the duration of store use.

inline std::pair<TempWAL, std::unique_ptr<KeyValueStore>> make_kv() {
    TempWAL tmp;
    auto storage = std::make_unique<InMemoryStorage>();
    auto wal     = std::make_unique<WAL>(tmp.path());
    auto kv      = std::make_unique<KeyValueStore>(std::move(storage),
                                                   std::move(wal));
    return { std::move(tmp), std::move(kv) };
}

// =============================================================================
// Deterministic key/value generators
// =============================================================================
//
// Keys: "key_%010llu" — zero-padded decimal for lexicographic ordering
// Values: repeated pattern of 'A'–'Z' to fill value_size bytes

inline std::string make_key(std::uint64_t index) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%010llu",
                  static_cast<unsigned long long>(index));
    return buf;
}

inline std::string make_value(std::uint32_t value_size, std::uint64_t seed = 0) {
    std::string v;
    v.reserve(value_size);
    for (std::uint32_t i = 0; i < value_size; ++i) {
        v.push_back(static_cast<char>('A' + (seed + i) % 26));
    }
    return v;
}

// =============================================================================
// Latency statistics — from a sorted vector of per-operation microsecond times
// =============================================================================

struct LatencyStats {
    double avg_us = 0.0;
    double p50_us = 0.0;
    double p95_us = 0.0;
    double p99_us = 0.0;
};

inline LatencyStats compute_latency(std::vector<double>& samples) {
    if (samples.empty()) return {};
    std::sort(samples.begin(), samples.end());
    const std::size_t n = samples.size();
    LatencyStats s;
    s.avg_us = std::accumulate(samples.begin(), samples.end(), 0.0) / static_cast<double>(n);
    s.p50_us = samples[n * 50 / 100];
    s.p95_us = samples[n * 95 / 100];
    s.p99_us = samples[std::min(n - 1, n * 99 / 100)];
    return s;
}

// =============================================================================
// Output helpers
// =============================================================================

inline void print_separator(char ch = '-', int width = 70) {
    std::cout << std::string(static_cast<std::size_t>(width), ch) << '\n';
}

inline void print_header(const std::string& title) {
    std::cout << '\n';
    print_separator('=');
    std::cout << title << '\n';
    print_separator('=');
}

inline void print_section(const std::string& title) {
    std::cout << '\n' << title << '\n';
    print_separator();
}

inline void print_throughput_table(const std::vector<BenchResult>& results) {
    std::cout << std::left
              << std::setw(30) << "Workload"
              << std::right
              << std::setw(12) << "Ops"
              << std::setw(12) << "Time(s)"
              << std::setw(14) << "Ops/sec"
              << '\n';
    print_separator();
    for (const auto& r : results) {
        std::cout << std::left  << std::setw(30) << r.workload
                  << std::right << std::setw(12) << r.ops
                  << std::setw(12) << std::fixed << std::setprecision(3) << r.elapsed_s
                  << std::setw(14) << std::fixed << std::setprecision(0) << r.ops_per_sec
                  << '\n';
    }
}

inline void print_concurrency_table(const std::vector<BenchResult>& results) {
    std::cout << std::right
              << std::setw(10) << "Threads"
              << std::setw(14) << "Ops"
              << std::setw(12) << "Time(s)"
              << std::setw(16) << "Ops/sec"
              << '\n';
    print_separator();
    for (const auto& r : results) {
        std::cout << std::setw(10) << r.threads
                  << std::setw(14) << r.ops
                  << std::setw(12) << std::fixed << std::setprecision(3) << r.elapsed_s
                  << std::setw(16) << std::fixed << std::setprecision(0) << r.ops_per_sec
                  << '\n';
    }
}

inline void print_latency_table(const std::vector<BenchResult>& results) {
    std::cout << std::left  << std::setw(30) << "Workload"
              << std::right
              << std::setw(12) << "avg(us)"
              << std::setw(12) << "p50(us)"
              << std::setw(12) << "p95(us)"
              << std::setw(12) << "p99(us)"
              << '\n';
    print_separator();
    for (const auto& r : results) {
        if (r.lat_p50_us == 0.0 && r.lat_avg_us == 0.0) continue;
        std::cout << std::left  << std::setw(30) << r.workload
                  << std::right
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.lat_avg_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.lat_p50_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.lat_p95_us
                  << std::setw(12) << std::fixed << std::setprecision(2) << r.lat_p99_us
                  << '\n';
    }
}

inline void print_wal_table(const std::vector<BenchResult>& results) {
    std::cout << std::left  << std::setw(30) << "Workload"
              << std::right
              << std::setw(18) << "WAL Before(bytes)"
              << std::setw(18) << "WAL After(bytes)"
              << '\n';
    print_separator();
    for (const auto& r : results) {
        if (r.wal_before == 0 && r.wal_after == 0) continue;
        std::cout << std::left  << std::setw(30) << r.workload
                  << std::right
                  << std::setw(18) << r.wal_before
                  << std::setw(18) << r.wal_after
                  << '\n';
    }
}

// =============================================================================
// CSV output
// =============================================================================

inline void write_csv(const std::string& path,
                      const std::vector<BenchResult>& results) {
    std::ofstream f(path);
    if (!f.is_open()) {
        std::cerr << "Warning: cannot open output file: " << path << '\n';
        return;
    }
    f << "workload,threads,ops,elapsed_s,ops_per_sec,"
         "lat_avg_us,lat_p50_us,lat_p95_us,lat_p99_us,"
         "wal_before_bytes,wal_after_bytes\n";
    for (const auto& r : results) {
        f << r.workload << ','
          << r.threads << ','
          << r.ops << ','
          << std::fixed << std::setprecision(6) << r.elapsed_s << ','
          << std::fixed << std::setprecision(2) << r.ops_per_sec << ','
          << std::fixed << std::setprecision(2) << r.lat_avg_us << ','
          << std::fixed << std::setprecision(2) << r.lat_p50_us << ','
          << std::fixed << std::setprecision(2) << r.lat_p95_us << ','
          << std::fixed << std::setprecision(2) << r.lat_p99_us << ','
          << r.wal_before << ','
          << r.wal_after << '\n';
    }
    std::cout << "\nResults written to: " << path << '\n';
}

// =============================================================================
// JSON output (Stage 18 — machine-readable benchmark artifact for the frontend)
//
// Schema version: 1
//
// Outputs a self-describing JSON document to the given path (or stdout if
// path is empty).  The frontend reads this file from the public/ directory.
//
// Format:
//   {
//     "schema_version": 1,
//     "forgekv_version": "0.13.0",
//     "generated_at": "<ISO-8601 UTC timestamp>",
//     "environment": {
//       "os": "macOS" | "Linux" | "Windows" | "Unknown",
//       "compiler": "<compiler string>",
//       "cpp_standard": "C++20",
//       "hw_threads": <N>
//     },
//     "config": {
//       "operations": <N>,
//       "warmup": <N>,
//       "value_size_bytes": <N>,
//       "max_threads": <N>,
//       "latency_enabled": true | false,
//       "http_enabled": true | false
//     },
//     "workloads": [
//       {
//         "name": "<workload name>",
//         "threads": <N>,
//         "ops": <N>,
//         "elapsed_s": <double>,
//         "ops_per_sec": <double>,
//         "latency_us": {
//           "avg": <double>,  -- 0 if not measured
//           "p50": <double>,
//           "p95": <double>,
//           "p99": <double>
//         },
//         "wal_before_bytes": <N>,
//         "wal_after_bytes": <N>
//       },
//       ...
//     ]
//   }
//
// Units:
//   - ops_per_sec: operations per second (throughput)
//   - elapsed_s:   seconds (double)
//   - latency_us:  microseconds (double) — 0 means "not measured"
//   - wal_*_bytes: bytes (uint64)
//
// NOTE: latency numbers come from per-operation timing with individual Timer
// calls; this includes timer overhead (~nanoseconds per sample on modern
// hardware). The numbers are benchmark measurements, not live server metrics.
// =============================================================================

// Minimal JSON string escape: handles \, ", \n, \r, \t, and control chars.
inline std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    // Control character: encode as \uXXXX.
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Return current UTC time as an ISO-8601 string (seconds precision).
inline std::string iso8601_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf{};
#if defined(_WIN32)
    gmtime_s(&tm_buf, &t);
#else
    gmtime_r(&t, &tm_buf);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);
    return buf;
}

inline void write_json(const std::string& path,
                       const std::vector<BenchResult>& results,
                       const BenchConfig& cfg) {
    std::ostringstream j;

    // Helper lambdas for indented emission.
    auto dbl = [&](double v, int prec = 2) -> std::string {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(prec) << v;
        return ss.str();
    };

    j << "{\n";
    j << "  \"schema_version\": 1,\n";
    j << "  \"forgekv_version\": \"0.13.0\",\n";
    j << "  \"generated_at\": \"" << iso8601_now() << "\",\n";

    // Environment
    j << "  \"environment\": {\n";
#if defined(__APPLE__)
    j << "    \"os\": \"macOS\",\n";
#elif defined(__linux__)
    j << "    \"os\": \"Linux\",\n";
#elif defined(_WIN32)
    j << "    \"os\": \"Windows\",\n";
#else
    j << "    \"os\": \"Unknown\",\n";
#endif
#if defined(__clang__)
    j << "    \"compiler\": \"Clang " << __clang_major__ << "." << __clang_minor__ << "\",\n";
#elif defined(__GNUC__)
    j << "    \"compiler\": \"GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\",\n";
#else
    j << "    \"compiler\": \"Unknown\",\n";
#endif
    j << "    \"cpp_standard\": \"C++20\",\n";
    j << "    \"hw_threads\": " << std::thread::hardware_concurrency() << "\n";
    j << "  },\n";

    // Config
    j << "  \"config\": {\n";
    j << "    \"operations\": "        << cfg.operations  << ",\n";
    j << "    \"warmup\": "            << cfg.warmup      << ",\n";
    j << "    \"value_size_bytes\": "  << cfg.value_size  << ",\n";
    j << "    \"max_threads\": "       << cfg.threads     << ",\n";
    j << "    \"latency_enabled\": "   << (cfg.latency   ? "true" : "false") << ",\n";
    j << "    \"http_enabled\": "      << (cfg.run_http  ? "true" : "false")  << "\n";
    j << "  },\n";

    // Workloads array
    j << "  \"workloads\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const bool last = (i + 1 == results.size());
        j << "    {\n";
        j << "      \"name\": \""        << json_escape(r.workload) << "\",\n";
        j << "      \"threads\": "       << r.threads << ",\n";
        j << "      \"ops\": "           << r.ops << ",\n";
        j << "      \"elapsed_s\": "     << dbl(r.elapsed_s, 6) << ",\n";
        j << "      \"ops_per_sec\": "   << dbl(r.ops_per_sec, 2) << ",\n";
        j << "      \"latency_us\": {\n";
        j << "        \"avg\": " << dbl(r.lat_avg_us, 2) << ",\n";
        j << "        \"p50\": " << dbl(r.lat_p50_us, 2) << ",\n";
        j << "        \"p95\": " << dbl(r.lat_p95_us, 2) << ",\n";
        j << "        \"p99\": " << dbl(r.lat_p99_us, 2) << "\n";
        j << "      },\n";
        j << "      \"wal_before_bytes\": " << r.wal_before << ",\n";
        j << "      \"wal_after_bytes\": "  << r.wal_after  << "\n";
        j << "    }" << (last ? "" : ",") << "\n";
    }
    j << "  ]\n";
    j << "}\n";

    const std::string content = j.str();

    if (path.empty()) {
        // Write to stdout (useful for piping: ./forgekv_benchmark --format json > results.json)
        std::cout << content;
    } else {
        std::ofstream f(path);
        if (!f.is_open()) {
            std::cerr << "Warning: cannot open JSON output file: " << path << '\n';
            return;
        }
        f << content;
        std::cout << "\nJSON results written to: " << path << '\n';
    }
}

// =============================================================================
// CLI argument parsing
// =============================================================================

inline BenchConfig parse_args(int argc, char* argv[]) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--operations" || arg == "-n") && i + 1 < argc) {
            cfg.operations = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        } else if ((arg == "--warmup" || arg == "-w") && i + 1 < argc) {
            cfg.warmup = static_cast<std::uint64_t>(std::stoull(argv[++i]));
        } else if ((arg == "--value-size" || arg == "-v") && i + 1 < argc) {
            cfg.value_size = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if ((arg == "--threads" || arg == "-t") && i + 1 < argc) {
            cfg.threads = static_cast<std::uint32_t>(std::stoul(argv[++i]));
        } else if ((arg == "--output" || arg == "-o") && i + 1 < argc) {
            cfg.output_file = argv[++i];
        } else if (arg == "--json-output" && i + 1 < argc) {
            // --json-output <file>  — write JSON results to file
            cfg.json_output_file = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            // --format json  — write JSON results to stdout ("-")
            std::string fmt = argv[++i];
            if (fmt == "json") {
                cfg.json_output_file = "-";  // "-" = stdout
            } else if (fmt != "csv") {
                std::cerr << "Unknown format: " << fmt
                          << " (supported: csv, json)\n";
            }
        } else if (arg == "--no-latency") {
            cfg.latency = false;
        } else if (arg == "--no-http") {
            cfg.run_http = false;
        } else if (arg == "--large") {
            cfg.run_large = true;
        } else if (arg == "--help" || arg == "-h") {
            std::cout <<
R"(ForgeKV Benchmark — Stage 12

Usage: forgekv_benchmark [options]

Options:
  --operations, -n <N>      Number of operations per workload (default: 100000)
  --warmup,     -w <N>      Warmup operations (not measured)   (default: 1000)
  --value-size, -v <N>      Value payload size in bytes        (default: 128)
  --threads,    -t <N>      Max threads for concurrency bench  (default: 4)
  --output,     -o <file>   Write results to CSV file
  --json-output    <file>   Write results to JSON file
  --format         json     Write JSON results to stdout
  --no-latency              Skip latency sampling
  --no-http                 Skip HTTP benchmark
  --large                   Also run 1,000,000-operation suite
  --help,       -h          Show this help

Examples:
  ./forgekv_benchmark
  ./forgekv_benchmark --operations 50000 --threads 8
  ./forgekv_benchmark --output results.csv
  ./forgekv_benchmark --json-output benchmark-results.json
  ./forgekv_benchmark --format json > benchmark-results.json
  ./forgekv_benchmark --value-size 1024 --no-http
)";
            std::exit(0);
        } else {
            std::cerr << "Unknown argument: " << arg
                      << " (use --help for usage)\n";
        }
    }
    return cfg;
}

// =============================================================================
// Environment info
// =============================================================================

inline void print_env_info() {
    print_section("Environment");

#if defined(__APPLE__)
    std::cout << "  OS            : macOS\n";
#elif defined(__linux__)
    std::cout << "  OS            : Linux\n";
#elif defined(_WIN32)
    std::cout << "  OS            : Windows\n";
#else
    std::cout << "  OS            : Unknown\n";
#endif

#if defined(__clang__)
    std::cout << "  Compiler      : Clang " << __clang_major__
              << "." << __clang_minor__ << '\n';
#elif defined(__GNUC__)
    std::cout << "  Compiler      : GCC " << __GNUC__
              << "." << __GNUC_MINOR__ << '\n';
#else
    std::cout << "  Compiler      : Unknown\n";
#endif

    std::cout << "  C++ standard  : C++20\n";
    std::cout << "  HW threads    : " << std::thread::hardware_concurrency() << '\n';
}

inline void print_config(const BenchConfig& cfg) {
    print_section("Configuration");
    std::cout << "  Operations    : " << cfg.operations   << '\n';
    std::cout << "  Warmup        : " << cfg.warmup       << '\n';
    std::cout << "  Value size    : " << cfg.value_size   << " bytes\n";
    std::cout << "  Max threads   : " << cfg.threads      << '\n';
    std::cout << "  Latency       : " << (cfg.latency ? "yes" : "no") << '\n';
    std::cout << "  HTTP bench    : " << (cfg.run_http ? "yes" : "no") << '\n';
    std::cout << "  Output file   : "
              << (cfg.output_file.empty() ? "(none)" : cfg.output_file) << '\n';
}

} // namespace forgekv::bench
