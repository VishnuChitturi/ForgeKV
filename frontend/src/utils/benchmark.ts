// =============================================================================
// ForgeKV Stage 18 — Benchmark data loader and query utilities
//
// loadBenchmarkResults:
//   Fetches /benchmark-results.json from the frontend public directory.
//   Returns a typed BenchmarkResults object on success, or a reason string
//   on failure. Never throws — all errors are caught and returned.
//
// Helper functions:
//   findWorkload     — look up a single workload by name (exact match)
//   findWorkloads    — look up all workloads whose names include a substring
//   formatOpsPerSec  — format throughput as "394.9K" or "6.45M"
//   formatLatencyUs  — format a microsecond value with explicit unit label
// =============================================================================

import type { BenchmarkResults, BenchWorkload } from "../types/benchmark";

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

/**
 * Fetch the benchmark artifact from /benchmark-results.json.
 *
 * Returns:
 *   { ok: true, data }           on success
 *   { ok: false, reason: string } on any failure (network, parse, missing)
 *
 * The file is served from the frontend/public/ directory at build time, or
 * via the Vite dev server during development.  It is NOT generated on demand.
 */
export async function loadBenchmarkResults(): Promise<
  { ok: true; data: BenchmarkResults } | { ok: false; reason: string }
> {
  try {
    const res = await fetch("/benchmark-results.json", {
      // Disable cache in dev so a freshly-generated file is always picked up.
      cache: "no-cache",
    });

    if (res.status === 404) {
      return {
        ok: false,
        reason:
          "benchmark-results.json not found. " +
          "Run the benchmark suite and copy the output to frontend/public/:\n" +
          "  ./build/forgekv_benchmark --json-output frontend/public/benchmark-results.json",
      };
    }

    if (!res.ok) {
      return {
        ok: false,
        reason: `Failed to load benchmark results (HTTP ${res.status}).`,
      };
    }

    const text = await res.text();
    let parsed: unknown;
    try {
      parsed = JSON.parse(text);
    } catch {
      return { ok: false, reason: "benchmark-results.json contains invalid JSON." };
    }

    // Basic shape validation — enough to give a useful error message.
    if (!parsed || typeof parsed !== "object") {
      return { ok: false, reason: "benchmark-results.json has unexpected format (not an object)." };
    }
    const obj = parsed as Record<string, unknown>;
    if (obj["schema_version"] !== 1) {
      return {
        ok: false,
        reason: `Unsupported benchmark schema version: ${String(obj["schema_version"])}. Expected 1.`,
      };
    }
    if (!Array.isArray(obj["workloads"])) {
      return { ok: false, reason: "benchmark-results.json is missing the 'workloads' array." };
    }

    return { ok: true, data: parsed as BenchmarkResults };
  } catch (err) {
    const msg = err instanceof Error ? err.message : "Network error";
    return { ok: false, reason: `Could not load benchmark results: ${msg}` };
  }
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

/** Find the first workload whose name exactly matches the given string. */
export function findWorkload(
  workloads: BenchWorkload[],
  name: string
): BenchWorkload | undefined {
  return workloads.find((w) => w.name === name);
}

/**
 * Find all workloads whose name includes the given substring (case-insensitive).
 * Useful for grouping, e.g., findWorkloads(ws, "Concurrent GET").
 */
export function findWorkloads(
  workloads: BenchWorkload[],
  nameFragment: string
): BenchWorkload[] {
  const lc = nameFragment.toLowerCase();
  return workloads.filter((w) => w.name.toLowerCase().includes(lc));
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

/**
 * Format ops/sec with K/M suffix for compact display.
 *
 * Examples:
 *   394900   → "394.9K"
 *   6450000  → "6.45M"
 *   1200     → "1,200"
 *   0        → "—"      (non-throughput workload like Snapshot/Compaction)
 */
export function formatOpsPerSec(opsPerSec: number): string {
  if (opsPerSec <= 0) return "—";
  if (opsPerSec >= 1_000_000) {
    return `${(opsPerSec / 1_000_000).toFixed(2)}M`;
  }
  if (opsPerSec >= 1_000) {
    return `${(opsPerSec / 1_000).toFixed(1)}K`;
  }
  return opsPerSec.toFixed(0);
}

/**
 * Format a latency value in microseconds with the appropriate unit.
 *
 * - 0 → "—"  (not measured)
 * - < 1000 µs → "X.XX µs"
 * - >= 1000 µs → "X.XX ms"
 */
export function formatLatencyUs(us: number): string {
  if (us <= 0) return "—";
  if (us < 1_000) return `${us.toFixed(2)} µs`;
  return `${(us / 1_000).toFixed(2)} ms`;
}

/**
 * Format elapsed seconds as a human-readable duration.
 *
 * - < 0.001 s → "< 1 ms"
 * - < 1 s     → "X.X ms"
 * - >= 1 s    → "X.XX s"
 */
export function formatElapsed(seconds: number): string {
  if (seconds <= 0) return "—";
  if (seconds < 0.001) return "< 1 ms";
  if (seconds < 1) return `${(seconds * 1000).toFixed(1)} ms`;
  return `${seconds.toFixed(2)} s`;
}

/**
 * Format WAL size reduction percentage for the compaction workload.
 * Returns "—" if wal_before_bytes is 0.
 */
export function formatWalReduction(before: number, after: number): string {
  if (before <= 0) return "—";
  const pct = ((before - after) / before) * 100;
  return `${pct.toFixed(1)}%`;
}
