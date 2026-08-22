# 10 — Benchmarking

> **Stage:** 12  
> **Status:** 🔲 Planned. This document describes the benchmarking strategy. No numbers exist yet.

> **Stage 20 note:** This is an early planning document written before Stage 12
> was implemented. The actual benchmark implementation is documented in
> `docs/12-benchmarking.md`. The benchmark executable is `build/forgekv_benchmark`;
> the pre-generated JSON artifact is at `frontend/public/benchmark-results.json`.
> See `README.md` for verified build and run instructions.

---

## Principle: Measure, Don't Estimate

ForgeKV will not publish performance numbers until they are measured from the actual implementation running on real hardware. Invented or extrapolated numbers are worse than no numbers — they create false expectations and cannot be reproduced or challenged.

This document describes **what** will be measured and **how**. The actual results will be filled in after Stage 12 is implemented.

---

## Why Benchmark?

Benchmarking serves two purposes in a learning project like ForgeKV:

1. **Verification.** Each stage introduces overhead. Benchmarks verify that new components (WAL, locking, HTTP) have overhead consistent with expectations. An unexpectedly slow result signals a design or implementation problem.

2. **Documentation.** A real benchmark suite with reproducible methodology is more useful in a portfolio than claimed performance numbers. It demonstrates the ability to measure, reason about, and improve performance.

---

## Metrics

### Throughput

Throughput measures how many operations the system can complete per unit of time.

| Metric           | Unit                         |
|------------------|------------------------------|
| GET throughput   | operations per second (ops/s) |
| SET throughput   | operations per second (ops/s) |
| DELETE throughput | operations per second (ops/s) |

Throughput is measured with a fixed number of operations executed as fast as possible, then divided by elapsed wall-clock time.

### Latency

Latency measures how long a single operation takes from the caller's perspective.

| Metric      | Description                                          |
|-------------|------------------------------------------------------|
| Average     | Mean latency across all operations                   |
| P50         | 50th percentile — half of operations are faster      |
| P95         | 95th percentile — 95% of operations are faster       |
| P99         | 99th percentile — 99% of operations are faster       |

P99 is the most important latency metric for a storage system. Tail latency (slow outliers) matters in practice because a single slow request can block a client.

Latency is measured per-operation and sorted to compute percentiles.

### Memory Usage

| Metric                        | Description                              |
|-------------------------------|------------------------------------------|
| Memory usage vs. key count    | How much RAM the store consumes          |
| Memory per key-value pair     | Average overhead per entry               |

Measured via process memory tracking (e.g., `/proc/self/status` on Linux or `getrusage` on macOS).

### WAL Size

| Metric                          | Description                                  |
|---------------------------------|----------------------------------------------|
| WAL file size vs. operation count | How fast the WAL grows                      |
| WAL size before and after compaction | Space savings from compaction            |

### Recovery Time

| Metric                    | Description                                    |
|---------------------------|------------------------------------------------|
| Recovery time vs. WAL size | How long startup takes with a given WAL       |
| Recovery time with snapshot | Startup time with snapshot + short WAL delta  |

### Compaction Time

| Metric              | Description                              |
|---------------------|------------------------------------------|
| Compaction duration | Wall-clock time to compact the WAL       |
| Compaction savings  | Bytes freed as a fraction of WAL size    |

---

## Benchmark Scenarios

### Scenario 1: In-Memory Baseline

Operations against the in-memory store with no WAL, no HTTP, no concurrency. This establishes the performance ceiling.

```
Measure: raw GET/SET/DELETE throughput and latency
Store size: small (1K keys), medium (100K keys), large (1M keys)
```

### Scenario 2: WAL Overhead

Operations with the WAL enabled. Compares against the in-memory baseline to quantify durability cost.

```
Measure: SET throughput with WAL vs. without WAL
Varies: with fsync, without fsync (to isolate I/O cost)
```

### Scenario 3: Concurrent Reads

Multiple reader threads accessing the store simultaneously.

```
Measure: aggregate GET throughput as thread count increases (1, 2, 4, 8, 16)
Expected: throughput increases because GET uses shared lock
```

### Scenario 4: Concurrent Writes

Multiple writer threads.

```
Measure: aggregate SET throughput as thread count increases
Expected: throughput may not scale linearly due to exclusive lock
```

### Scenario 5: Mixed Read/Write

Simulates a realistic workload.

```
Mix: 80% GET, 20% SET
Thread count: varied
Measure: throughput and P99 latency
```

### Scenario 6: Recovery

```
Prepare: WAL with N records (various N values)
Measure: startup time to full recovery vs. N
```

### Scenario 7: HTTP Overhead

```
Measure: end-to-end latency for GET/SET over HTTP vs. direct function call
Quantifies the cost of the network layer
```

---

## Methodology

All benchmarks will:

- Run on dedicated hardware with no other significant workload
- Report the hardware configuration (CPU, RAM, storage type)
- Perform a warm-up period before measurement
- Report wall-clock time, not CPU time (wall time is what clients experience)
- Be reproducible: the benchmark code will be in the `benchmarks/` directory

No benchmark results will be published in this document until Stage 12 is implemented.

---

## Benchmark Infrastructure

The benchmark suite will live in `benchmarks/`. Options being considered:

- **Custom harness** — a minimal C++ benchmark runner, sufficient for ForgeKV's needs
- **Google Benchmark** — a well-known C++ microbenchmark library

The choice will be made at Stage 12 implementation time, with the same dependency-justification standard applied to all ForgeKV dependencies.

---

## Result Placeholder

> **No benchmark results are published here.**  
> Results will be added after Stage 12 is implemented and measurements are taken on real hardware.
>
> Any performance numbers seen in other sections of this documentation are illustrative only, not measured results.

---

*Previous: [09-ttl.md](09-ttl.md)*  
*Next: [11-testing.md](11-testing.md)*
