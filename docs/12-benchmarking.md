# Stage 12 — Benchmarking

ForgeKV v0.12.0

---

## Overview

Stage 12 adds a benchmark suite to ForgeKV to measure real, reproducible performance on actual hardware. The goal of this stage is **measurement**, not optimization. The benchmark establishes a baseline that future changes can be compared against.

No performance numbers were invented or estimated in advance. All numbers in this document are obtained by running `./build/forgekv_benchmark` and observing actual output.

---

## Table of Contents

1. [Benchmark Architecture](#benchmark-architecture)
2. [Files Added](#files-added)
3. [How to Build](#how-to-build)
4. [How to Run](#how-to-run)
5. [Command-Line Options](#command-line-options)
6. [Workloads](#workloads)
7. [Timing Methodology](#timing-methodology)
8. [Warmup Methodology](#warmup-methodology)
9. [Concurrency Methodology](#concurrency-methodology)
10. [Latency Methodology](#latency-methodology)
11. [Correctness Verification](#correctness-verification)
12. [WAL Measurement](#wal-measurement)
13. [HTTP Benchmark](#http-benchmark)
14. [Example Output](#example-output)
15. [Limitations](#limitations)
16. [Why Results Depend on Hardware and Environment](#why-results-depend-on-hardware-and-environment)

---

## Benchmark Architecture

The benchmark suite uses a custom lightweight harness (`benchmarks/bench_harness.h`). No external benchmarking framework was introduced.

Key components:

**BenchConfig**
Runtime-configurable parameters parsed from CLI arguments. Fields: `operations`, `warmup`, `value_size`, `threads`, `latency`, `latency_samples`, `output_file`, `run_http`.

**TempWAL**
RAII guard that creates a unique temporary WAL file path in `/tmp` on construction and deletes the file (and its `.snapshot` sibling) on destruction. Every benchmark workload gets its own isolated `KeyValueStore` backed by a fresh `TempWAL`. This prevents state bleed between workloads.

**Timer**
Wraps `std::chrono::steady_clock`. Call `start()` before the workload, `elapsed_s()` after. Used exclusively for timing — no other clock is used in the measurement path.

**BenchResult**
Plain struct capturing: workload name, operation count, elapsed seconds, ops/sec, WAL sizes, thread count, and optional latency percentiles.

**Isolation**
Each benchmark creates a `KeyValueStore` via the dependency-injection constructor:
```cpp
KeyValueStore(std::unique_ptr<Storage>, std::unique_ptr<WAL>)
```
This is the same pattern used by the test suite. Benchmarks never touch the default `forgekv.wal` in the working directory.

---

## Files Added

| File | Purpose |
|------|---------|
| `benchmarks/bench_harness.h` | Harness: config, timer, TempWAL, output helpers, CSV writer, arg parser |
| `benchmarks/benchmark_kv_store.cpp` | KV-level workloads (A–H), concurrency, latency |
| `benchmarks/benchmark_http.cpp` | HTTP-level workloads (GET, PUT, Mixed) |
| `benchmarks/benchmark_main.cpp` | Entry point, orchestrates all benchmarks, prints summary |
| `docs/12-benchmarking.md` | This document |

**Files modified:**

| File | Change |
|------|--------|
| `CMakeLists.txt` | Version `0.11.0` → `0.12.0`; added `forgekv_benchmark` target |
| `README.md` | Updated status, roadmap table, benchmarking strategy section |

---

## How to Build

```bash
# From the project root:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Or for a debug build (default):
cmake -B build
cmake --build build
```

The benchmark target is `forgekv_benchmark`. The normal test targets (`forgekv_tests`, `forgekv_http_tests`) build as usual.

For meaningful throughput numbers, prefer a Release build:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target forgekv_benchmark
```

---

## How to Run

```bash
# Default run (100,000 ops, 128-byte values, up to 4 threads):
./build/forgekv_benchmark

# Custom operations and threads:
./build/forgekv_benchmark --operations 50000 --threads 8

# Save results to CSV:
./build/forgekv_benchmark --output results.csv

# Skip HTTP benchmark (faster):
./build/forgekv_benchmark --no-http

# Skip latency sampling:
./build/forgekv_benchmark --no-latency

# 1 KB values:
./build/forgekv_benchmark --value-size 1024

# Show help:
./build/forgekv_benchmark --help
```

The benchmark does **not** run automatically during `ctest`. It is intentionally excluded from the CTest suite.

---

## Command-Line Options

| Option | Short | Default | Description |
|--------|-------|---------|-------------|
| `--operations` | `-n` | 100000 | Operations per workload (measured phase) |
| `--warmup` | `-w` | 1000 | Warmup operations (not measured) |
| `--value-size` | `-v` | 128 | Value payload size in bytes |
| `--threads` | `-t` | 4 | Max thread count for concurrency benchmarks |
| `--output` | `-o` | (none) | Write results to a CSV file at this path |
| `--no-latency` | | latency on | Skip per-operation latency sampling |
| `--no-http` | | HTTP on | Skip HTTP benchmark |
| `--large` | | off | Also run a 1,000,000-operation suite |
| `--help` | `-h` | | Show usage and exit |

**Notes on value sizes:**

The default 128-byte value is a reasonable general-purpose size. For small-record benchmarks use `--value-size 16`. For large-record benchmarks use `--value-size 1024`. Running all three sizes by default would triple benchmark runtime; use `--value-size` to select the size of interest.

**Notes on thread counts:**

The benchmark automatically caps thread counts at `std::thread::hardware_concurrency()`. Specifying `--threads 8` on a 4-core machine will cap at 4. Thread count progression is always a power-of-2 ladder from 1 to the cap (e.g., 1, 2, 4).

---

## Workloads

### A. Sequential SET

- Performs `operations` unique `set(key, value)` calls.
- Keys are deterministic: `key_0000000000`, `key_0000000001`, etc.
- Values are deterministic: repeated `A–Z` pattern of length `value_size`.
- Warmup runs, then store is reset so warmup state does not appear in the WAL.
- **Measures:** throughput (ops/sec), WAL growth.

### B. Sequential GET HIT

- Pre-populates all keys (outside timing).
- Reads every key in order.
- All reads should be cache-warm hits.
- **Measures:** read throughput, hit count.

### C. Sequential GET MISS

- Reads keys that do not exist (empty store).
- Warmup is miss-only — no state change.
- **Measures:** miss throughput.

### D. Sequential DELETE

- Pre-populates keys (outside timing).
- Deletes every key in order.
- **Measures:** delete throughput, final key count (should be zero), WAL growth.

### E. Mixed Workload

Distribution per operation `i`:

| `i % 10` | Operation | Target |
|----------|-----------|--------|
| 0–4 | GET | Key `i % ops` (mix of hit/miss) |
| 5–7 | SET | Key `i % ops` |
| 8 | DELETE | Key `i % ops` |
| 9 | GET MISS | Key `ops + i` (guaranteed absent) |

This gives approximately 50% GET / 30% SET / 10% DELETE / 10% GET MISS.

Half the keyspace is pre-populated before the measured phase so the GET operations have a meaningful hit rate.

### F. TTL Workload

- Calls `set_with_ttl(key, value, 3600.0)` — one-hour TTL.
- Long TTL ensures no expiration occurs during the benchmark.
- **Measures:** TTL-write throughput, WAL growth.
- Does **not** wait for expiration as part of the benchmark.

### G. Snapshot

- Pre-populates `min(operations, 10000)` keys.
- Measures `snapshot()` duration for that dataset.
- **Reports:** duration, dataset size, WAL size before/after, snapshot file size.
- This is a **duration benchmark**, not a throughput benchmark.

### H. Compaction

- Writes each of `min(operations / 10, 5000)` keys **5 times** to bloat the WAL.
- Measures `compact()` duration.
- **Reports:** WAL size before and after, reduction ratio, duration.
- This is a **duration benchmark**, not a throughput benchmark.

### I. Concurrency

Three sub-workloads: concurrent GET, concurrent SET, concurrent Mixed (50% GET / 50% SET). Each sub-workload runs at 1, 2, 4, and up to `max_threads` hardware threads.

The total operation count is divided evenly among worker threads. All threads start the measured phase simultaneously (synchronized by `std::barrier`).

### J. Latency

Single-threaded workload where every individual operation is timed with `Timer::elapsed_us()`. After all operations, samples are sorted and percentiles computed.

Measured workloads: Latency SET, Latency GET.

**Caution:** The act of timing each operation individually adds overhead. Latency numbers include this measurement overhead and are not nanosecond-accurate. They give a practical sense of per-call cost, not a precise CPU cycle count.

---

## Timing Methodology

Every measured section uses `std::chrono::steady_clock` via the `Timer` class.

```
Timer t;
t.start();
// ... workload ...
double elapsed = t.elapsed_s();
```

`steady_clock` is monotonic — it never goes backward and is unaffected by system clock adjustments. It is the correct choice for elapsed-time measurement.

**What is inside timing:**
- The loop body for each operation: `set()`, `get()`, `del()`, `set_with_ttl()`.
- For concurrency benchmarks: the full parallel phase from the barrier arrival to the last `join()`.
- For snapshot/compaction: the single call to `snapshot()` or `compact()`.

**What is outside timing:**
- Store construction and WAL opening.
- Pre-population of keys (for GET/DELETE benchmarks).
- Warmup operations.
- Correctness verification.
- Stats collection.
- CSV output.
- Store destruction.

Throughput is computed as:
```
ops_per_sec = operations / elapsed_seconds
```

---

## Warmup Methodology

Warmup runs the same operation type as the measured workload but does **not** contribute to the measured elapsed time.

For workloads where warmup would pollute the initial store state (e.g., Sequential SET — warmup keys would inflate the WAL), the store is **reset** after warmup: the `KeyValueStore` and its `TempWAL` are destroyed and recreated. The measured phase then starts from a clean state.

For workloads where warmup has no persistent side effect (e.g., GET MISS on an empty store), the store is not reset.

Default warmup: `--warmup 1000`. Warmup can be disabled by passing `--warmup 0`.

---

## Concurrency Methodology

All concurrency benchmarks use `std::barrier` to synchronize worker threads before the measured phase begins.

```
Barrier arrival order:

  Thread 0     ─────────────────────────────────►  arrives at barrier
  Thread 1     ────────────────────────────►  arrives at barrier
  Thread N-1   ───────────────────────►  arrives at barrier
  Main thread  ────────────────────►  arrives at barrier (starts timer)

                              ↓ ALL PROCEED SIMULTANEOUSLY

  All threads  ────────────────────────────────►  measured phase
```

The timing thread (main) arrives at the barrier together with the worker threads. The timer starts only after all workers have arrived and the barrier releases them. This ensures all threads start the measured phase at the same moment.

No `std::this_thread::sleep_for` or any arbitrary sleep is used to coordinate workers.

The total elapsed time is measured from the barrier release to the last worker's `join()`.

---

## Latency Methodology

For Latency SET and Latency GET, every operation is timed individually:

```cpp
Timer per_op;
per_op.start();
kv->set(key, val);
samples.push_back(per_op.elapsed_us());
```

After the loop, all samples are sorted in place. Percentiles are derived by index:

```
p50 = samples[n * 50 / 100]
p95 = samples[n * 95 / 100]
p99 = samples[min(n-1, n * 99 / 100)]
avg = sum / n
```

Units are microseconds.

**Measurement overhead:** `steady_clock::now()` itself takes on the order of nanoseconds to tens of nanoseconds on modern hardware. For sub-microsecond operations, this overhead is significant and will inflate the reported latency relative to the true operation time. The benchmark does **not** attempt to subtract this overhead. Latency numbers should be interpreted as "practical per-call cost including measurement overhead" rather than "pure operation latency."

Throughput numbers from the latency workload are lower than from the dedicated throughput workloads because of this per-op timing cost. The two should not be compared directly.

---

## Correctness Verification

After every workload, the benchmark verifies that the `KeyValueStore` is in the expected state. These checks happen **outside** the measured timing window.

| Workload | Check |
|----------|-------|
| Sequential SET | `key_count == operations`, `set_count == operations` |
| GET HIT | `hit_count == operations`, `stats.get_hits >= operations` |
| GET MISS | `miss_count == operations` |
| DELETE | `key_count == 0`, `delete_count == operations` |
| Mixed | `get_hits + get_misses > 0`, `set_count > 0`, `wal_size > 0` |
| TTL SET | `key_count == operations`, `ttl_set_count == operations`, sample key TTL > 0 |
| Snapshot | `snapshot() == true`, `key_count` preserved, `last_snapshot_time_us > 0` |
| Compaction | `key_count == expected`, spot-check keys exist, `wal_after < wal_before` |
| Concurrency SET/Mixed | At least one key survives concurrent writes |
| Concurrency GET | `get_hits > 0` |
| HTTP PUT | `/stats` endpoint responds with HTTP 200 |

A failure increments a global counter. The final summary reports either `ALL PASSED` or the number of failed checks. The benchmark exits with code 1 if any check failed.

---

## WAL Measurement

WAL sizes are obtained via `kv->stats().wal_size_bytes`, which calls `WAL::file_size()` — a `std::filesystem::file_size()` call on the WAL path. This returns the actual on-disk file size at that moment.

For throughput workloads: WAL size is recorded immediately before the measured phase and immediately after.

For snapshot and compaction: WAL size before and after is printed as part of the workload-specific output.

All SET and DELETE operations exercise the **real WAL** — no in-memory-only bypass is used. The WAL is appended on every `set()` / `del()` / `set_with_ttl()` call, consistent with the existing durability model.

ForgeKV does not call `fsync` on the WAL beyond what the standard library buffered flush provides. This is unchanged from Stage 3–11; the benchmark preserves this behavior and does not add or remove `fsync` calls.

---

## HTTP Benchmark

The HTTP benchmark starts a `forgekv::HttpServer` on an OS-assigned ephemeral port (via `bind_to_any_port("127.0.0.1")`), sends real HTTP requests using `httplib::Client`, and measures end-to-end HTTP throughput including the full network stack overhead on loopback.

Because HTTP adds significant per-request overhead (connection handling, header parsing, JSON serialization, response formatting) even on loopback, HTTP throughput is substantially lower than in-process KV throughput. This is expected and documented in the benchmark output.

**HTTP operation count:** To keep runtime reasonable, the HTTP benchmark uses `min(operations / 10, 5000)` operations by default. HTTP throughput at even 1000 ops/sec × 5000 ops = 5 seconds per workload; running the full 100,000-op count would take minutes.

**Server lifecycle:** Each HTTP workload creates a fresh server with its own `KeyValueStore` and `TempWAL`. The server runs in a background thread. All clients synchronize at a `std::barrier` before sending requests. The server is stopped and joined before the next workload.

No external HTTP load-testing tools (wrk, ab, hey) are used. The benchmark uses the same vendored `httplib.h` client that the existing HTTP integration tests use.

---

## Example Output

The following is **illustrative** of the output format. Actual numbers depend on hardware, build type, OS, and system load. Do not treat these as ForgeKV performance claims.

```
======================================================================
ForgeKV Benchmark — Stage 12
======================================================================

Environment
----------------------------------------------------------------------
  OS            : macOS
  Compiler      : Clang 16.0
  C++ standard  : C++20
  HW threads    : 10

Configuration
----------------------------------------------------------------------
  Operations    : 100000
  Warmup        : 1000
  Value size    : 128 bytes
  Max threads   : 4
  Latency       : yes
  HTTP bench    : yes
  Output file   : (none)

Sequential Throughput Benchmarks
----------------------------------------------------------------------
  Running: Sequential SET ...
  Running: Sequential GET HIT ...
  ...

Throughput Results
----------------------------------------------------------------------
Workload                          Ops      Time(s)       Ops/sec
----------------------------------------------------------------------
Sequential SET                 100000        X.XXX         XXXXX
Sequential GET (hit)           100000        X.XXX         XXXXX
Sequential GET (miss)          100000        X.XXX         XXXXX
Sequential DELETE              100000        X.XXX         XXXXX
Mixed (50G/30S/10D/10M)        100000        X.XXX         XXXXX
TTL SET (set_with_ttl)         100000        X.XXX         XXXXX

Snapshot Benchmark
----------------------------------------------------------------------
    Dataset: 10000 keys × 128 bytes
    Duration: X.XXXXXX s
    WAL before: XXXXX bytes
    WAL at snapshot: XXXXX bytes
    Snapshot file: XXXXX bytes

Compaction Benchmark
----------------------------------------------------------------------
    Keys: 5000 × 5 rewrites
    WAL before: XXXXXX bytes
    WAL after:  XXXXX bytes
    Reduction:  XX.X%
    Duration:   X.XXXXXX s

...

Final Summary
----------------------------------------------------------------------
  Total workloads measured : XX
  Correctness checks       : ALL PASSED
```

> **Note:** All numeric values above are placeholders (`X`). Run the benchmark on your hardware to obtain actual results. Numbers are machine-specific and will vary.

---

## Limitations

**Machine-dependent results:**
Throughput and latency numbers are specific to the machine, build type, OS scheduler, and disk I/O subsystem. Release builds (`-DCMAKE_BUILD_TYPE=Release`) are significantly faster than Debug builds.

**No fsync:**
ForgeKV does not call `fsync` on the WAL. Durability guarantees are limited to what the OS does automatically on normal process exit or disk flush. This is consistent across all stages and is not changed by the benchmark.

**Custom harness, not a full statistical framework:**
The harness does not compute standard deviation, run multiple trials and average them, or perform statistical significance testing. Each workload runs once. For a single run on an idle machine, numbers are reproducible; on a busy machine, variance may be significant.

**Latency measurement overhead:**
Per-operation timing using `steady_clock::now()` adds overhead. Latency numbers include this overhead and cannot be used to measure true operation latency on sub-microsecond workloads.

**HTTP benchmark is I/O-limited:**
HTTP throughput even on loopback is orders of magnitude lower than in-process throughput due to socket buffering, TCP, and HTTP parsing overhead. HTTP numbers are not a proxy for KV engine performance.

**Background cleanup thread:**
The `KeyValueStore` runs a 1-second background cleanup thread. For TTL benchmarks, this thread does not run during the benchmark (TTL is set to 1 hour and the benchmark completes in seconds). For other benchmarks, the thread wakes at most once per second and has no effect on results.

**Single snapshot file:**
ForgeKV maintains one current snapshot file per WAL path. The snapshot benchmark uses a separate TempWAL and does not affect any other benchmark.

**No distributed or multi-process benchmark:**
ForgeKV is a single-process, single-node engine. No distributed benchmarks are included or planned.

**No memory profiling:**
This benchmark measures throughput and latency but not memory usage. Memory profiling is outside scope for Stage 12.

---

## Why Results Depend on Hardware and Environment

Storage engines perform differently across machines because:

- **CPU clock speed and cache size** affect in-memory hash map performance.
- **Disk I/O latency** affects WAL append performance. SSD vs. HDD vs. tmpfs vs. RAM-backed `/tmp` all produce different WAL throughput.
- **OS scheduler and hardware concurrency** affect multi-threaded scalability.
- **Build type** (Debug vs. Release) affects instruction count dramatically.
- **System load** during the benchmark introduces variance.
- **Filesystem buffering** means file size queries reflect OS-visible state, not necessarily physically flushed state.

ForgeKV benchmarks should always be run on the machine of interest with a consistent build type. Results from one machine should not be assumed to hold on another.

---

*Stage 12 of ForgeKV — benchmarking infrastructure only. No production optimizations were made during this stage.*
