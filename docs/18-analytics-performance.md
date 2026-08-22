# Stage 18 — Analytics + Performance Dashboard

## Purpose

Stage 18 turns the `/admin` page into an operator-facing **Analytics + Performance** view.

It answers questions such as:

- How are GET operations distributed (hits vs. misses)?
- How much activity has the server handled since start?
- How are TTL and expiration behaving?
- What does the WAL and storage state look like?
- How fast is the storage engine (benchmark)?
- How does concurrency affect performance (benchmark)?
- How does the HTTP interface perform (benchmark)?

---

## Data Source Distinction

**This is the most important design decision in Stage 18.**

There are two fundamentally different kinds of data, and they must never be conflated:

| Data type | Source | Updated | How to refresh |
|-----------|--------|---------|----------------|
| **Live server metrics** | `GET /stats` | Manual refresh | Click "Refresh Live" button |
| **Benchmark results** | `benchmark-results.json` (static artifact) | At benchmark run time | Re-run the benchmark, copy file |

Benchmark results are **never** labeled as live metrics. The UI makes this explicit with:
- A section header clearly reading "Benchmark Performance"
- A `BenchmarkNotice` component at the top of every benchmark section stating these are controlled measurements
- "Benchmark ·" prefixes on all benchmark panel badges

---

## Live Server Metrics

All live metrics come from `GET /stats`. No new backend endpoints were added.

### Fields used

| Field | Display |
|-------|---------|
| `get_hits` | Hit rate numerator; GET hit count |
| `get_misses` | Hit rate denominator component; GET miss count |
| `set_count` | Operation breakdown |
| `delete_count` | Operation breakdown |
| `ttl_set_count` | Operation breakdown; TTL analytics |
| `expired_count` | Operation breakdown; TTL analytics |
| `key_count` | Live key count |
| `wal_size_bytes` | WAL size |
| `last_snapshot_time_us` | Last snapshot time (µs epoch) |
| `uptime_seconds` | Used by Stage 17 AdminHealth |

### GET Hit Rate

```
hit_rate = get_hits / (get_hits + get_misses) * 100
```

**Edge case:** if `get_hits + get_misses === 0`, shows `N/A` — never `100%`.

Displayed as a CSS SVG donut chart with the percentage in the centre.
The donut colour reflects the hit rate:
- ≥ 90% → green
- 60–89% → amber
- < 60% → red
- N/A → grey

### Operation Breakdown

Horizontal CSS bar chart showing cumulative counts since server start for:
- GET (hit)
- GET (miss)
- SET
- DELETE
- TTL SET
- Expired

Each bar's width is relative to the maximum value in the set.
Each row shows: label, bar, numeric count, percentage of total.

Bar colours are chosen to be distinguishable without relying only on hue.

### Persistence Analytics

Four metric blocks showing:
- **WAL Size** — `wal_size_bytes` formatted as B / KB / MB / GB
- **Live Keys** — `key_count` with thousand separators
- **Last Snapshot** — relative time ("3m ago") with absolute timestamp subtitle
- **Expired Keys** — `expired_count`

If `ttl_set_count > 0`, a TTL analytics row shows:
```
N keys set with TTL · M expired · X.X% expiry rate
```

---

## Benchmark Metrics

### Benchmark Artifact Format

Benchmark results are stored in a JSON file with the following schema (version 1):

```json
{
  "schema_version": 1,
  "forgekv_version": "0.13.0",
  "generated_at": "2026-08-22T15:30:00Z",
  "environment": {
    "os": "macOS",
    "compiler": "Clang 15.0",
    "cpp_standard": "C++20",
    "hw_threads": 10
  },
  "config": {
    "operations": 100000,
    "warmup": 1000,
    "value_size_bytes": 128,
    "max_threads": 4,
    "latency_enabled": true,
    "http_enabled": true
  },
  "workloads": [
    {
      "name": "Sequential SET",
      "threads": 1,
      "ops": 100000,
      "elapsed_s": 0.253400,
      "ops_per_sec": 394586.59,
      "latency_us": {
        "avg": 0.00,
        "p50": 0.00,
        "p95": 0.00,
        "p99": 0.00
      },
      "wal_before_bytes": 0,
      "wal_after_bytes": 14400000
    }
  ]
}
```

**Units:**
- `ops_per_sec` — operations per second (throughput)
- `elapsed_s` — seconds (double, 6 decimal places)
- `latency_us.*` — microseconds (double). `0` means "not measured for this workload"
- `wal_*_bytes` — bytes (uint64)

### Generating Benchmark Results

Run the benchmark binary with the `--json-output` flag:

```bash
# Generate and place in frontend public directory (recommended)
./build/forgekv_benchmark --json-output frontend/public/benchmark-results.json

# Or pipe to stdout and redirect
./build/forgekv_benchmark --format json > frontend/public/benchmark-results.json

# With options
./build/forgekv_benchmark \
  --operations 100000 \
  --threads 8 \
  --json-output frontend/public/benchmark-results.json
```

Then restart the dev server or rebuild the frontend to pick up the new file.

**Important:**
- Benchmark execution is a developer/operator action, not an automatic server operation.
- The production HTTP server never runs benchmarks.
- `/admin` never triggers benchmark execution.

### Frontend Loading

The frontend fetches `/benchmark-results.json` once on mount using `loadBenchmarkResults()` from `src/utils/benchmark.ts`.

- If the file is missing (404) → graceful empty state with instructions
- If the JSON is malformed → error message
- If `schema_version !== 1` → error message with the actual version found
- Loading is independent of live data refresh — clicking "Refresh Live" does **not** re-fetch the benchmark file

### Workloads Displayed

| Panel | Workload names |
|-------|----------------|
| BenchmarkOverview | Sequential SET, GET (hit), GET (miss), DELETE, Mixed, TTL SET |
| LatencyPanel | Latency SET, Latency GET |
| ConcurrencyPanel | Concurrent GET, Concurrent SET, Concurrent Mixed |
| HttpPerformancePanel | HTTP GET (hit), HTTP PUT, HTTP Mixed |
| SnapshotCompactionPanel | Snapshot, Compaction |

---

## Refresh Behavior

| Action | Effect on live data | Effect on benchmark data |
|--------|---------------------|--------------------------|
| Click "Refresh Live" | `GET /health` + `GET /stats` re-fetched | **No change** |
| Page load / mount | `GET /health` + `GET /stats` fetched | `benchmark-results.json` fetched once |
| Navigate away and back | Same as page load | Same as page load |

Benchmark data is loaded only once per page mount. This prevents inadvertent re-fetches and keeps the cost of opening `/admin` low.

---

## Visualization Approach

No third-party chart libraries were added. All visualizations use:

| Visualization | Technology |
|---------------|------------|
| GET hit rate donut | SVG `<circle>` with `strokeDasharray` |
| Operation breakdown bars | CSS `width` percentage |
| Concurrency scaling bars | CSS `width` percentage |
| All other data | HTML tables |

All visualizations:
- Have descriptive labels and numeric text values
- Work without color alone (values are always shown as text)
- Are responsive (tables scroll horizontally on mobile rather than breaking layout)
- Use semantic HTML (`<section>`, `<table>`, `<dl>`, `<article>`)
- Have `aria-label` attributes where appropriate

---

## Component Architecture

### Live analytics components

| Component | File | Purpose |
|-----------|------|---------|
| `AnalyticsOverview` | `components/AnalyticsOverview.tsx` | Grid container for HitRateCard + OperationBreakdown |
| `HitRateCard` | `components/HitRateCard.tsx` | SVG donut + hit/miss counts |
| `OperationBreakdown` | `components/OperationBreakdown.tsx` | CSS bar chart of cumulative op counters |
| `PersistenceAnalytics` | `components/PersistenceAnalytics.tsx` | WAL, keys, snapshot, TTL metrics |

### Benchmark components

| Component | File | Purpose |
|-----------|------|---------|
| `BenchmarkNotice` | `components/BenchmarkNotice.tsx` | Data source disclaimer / unavailable state |
| `BenchmarkOverview` | `components/BenchmarkOverview.tsx` | KV throughput table |
| `LatencyPanel` | `components/LatencyPanel.tsx` | p50/p95/p99 latency table |
| `ConcurrencyPanel` | `components/ConcurrencyPanel.tsx` | Thread-scaling tables |
| `HttpPerformancePanel` | `components/HttpPerformancePanel.tsx` | HTTP benchmark table |
| `SnapshotCompactionPanel` | `components/SnapshotCompactionPanel.tsx` | Snapshot + compaction timing |

### Utilities

| Module | File | Purpose |
|--------|------|---------|
| `BenchmarkResults` (type) | `types/benchmark.ts` | JSON schema types |
| `loadBenchmarkResults` | `utils/benchmark.ts` | Fetch + validate artifact |
| `findWorkload` / `findWorkloads` | `utils/benchmark.ts` | Query helpers |
| `formatOpsPerSec` | `utils/benchmark.ts` | "394.9K" / "6.45M" formatting |
| `formatLatencyUs` | `utils/benchmark.ts` | µs → "X.XX µs" or "X.XX ms" |
| `formatElapsed` | `utils/benchmark.ts` | seconds → "X.X ms" or "X.XX s" |
| `formatWalReduction` | `utils/benchmark.ts` | Compaction reduction percentage |

---

## API Changes

No production HTTP endpoints were added or modified.

The only change to the benchmark tooling:

- `bench_harness.h` — added `write_json()` function and `json_output_file` field to `BenchConfig`
- `benchmark_main.cpp` — added `--format json` and `--json-output <file>` flags

The production `forgekv_server` binary is unchanged.

---

## Error Handling

| Condition | Behavior |
|-----------|----------|
| Live stats fetch fails (first load) | Full-page error with Retry button |
| Live stats fetch fails (subsequent refresh) | Toast notification; last known data stays visible |
| benchmark-results.json not found (404) | `BenchmarkNotice` unavailable state with instructions |
| benchmark-results.json malformed JSON | Error message in unavailable state |
| benchmark-results.json wrong schema version | Error message with found version |
| Workload missing from results | "Not in benchmark results" row in table |

No fallback / fake values are shown in any error state.

---

## Responsive Behavior

| Viewport | Layout |
|----------|--------|
| Desktop (> 720px) | Multi-column analytics cards; tables side-by-side where applicable |
| Tablet (480–720px) | Cards wrap to one column; tables remain full-width |
| Mobile (< 480px) | Single column; tables scroll horizontally |

---

## Known Limitations

1. **Benchmark results reflect a single run.** There is no multi-run average or confidence interval. Outliers on the machine at benchmark time affect the numbers permanently.

2. **Machine-specific benchmark results.** Numbers from one machine cannot be directly compared to numbers from a different machine. The artifact includes `environment.os`, `environment.compiler`, and `environment.hw_threads` to provide context.

3. **No live latency tracking.** The backend does not expose per-request latency statistics. The LatencyPanel shows only benchmark-measured latency from the Stage 12 harness.

4. **HTTP benchmark is loopback-limited.** HTTP throughput numbers (HTTP GET, PUT, Mixed) are much lower than raw in-process KV throughput. This is expected and documented in the UI.

5. **Latency includes timer overhead.** The Stage 12 latency benchmark measures per-operation time with individual `Timer` calls. The `std::chrono::steady_clock` overhead (~nanoseconds) is included in each sample. This is noted in the LatencyPanel.

6. **No historical time-series.** Stage 18 shows current live stats and the most recent benchmark run. Historical trending is out of scope.

---

## Testing

Frontend:
```bash
cd frontend
npm run typecheck   # TypeScript type check
npm run build       # Production build
```

Backend (no changes to tests):
```bash
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

To generate a benchmark artifact for the frontend:
```bash
./build/forgekv_benchmark --json-output frontend/public/benchmark-results.json
```
