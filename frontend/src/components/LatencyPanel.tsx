// =============================================================================
// LatencyPanel — Stage 18 Benchmark Metrics
//
// Displays per-operation latency percentiles (avg, p50, p95, p99) measured by
// the Stage 12 benchmark suite for SET and GET operations.
//
// Data source: BENCHMARK ARTIFACT (benchmark-results.json)
// Units: MICROSECONDS — displayed as µs or ms as appropriate.
// These are benchmark measurements with timer overhead included.
// NOT live latency measurements from the running server.
// =============================================================================

import type { BenchmarkResults } from "../types/benchmark";
import { findWorkload, formatLatencyUs } from "../utils/benchmark";
import styles from "./LatencyPanel.module.css";

interface LatencyPanelProps {
  data: BenchmarkResults;
}

export function LatencyPanel({ data }: LatencyPanelProps) {
  const latSet = findWorkload(data.workloads, "Latency SET");
  const latGet = findWorkload(data.workloads, "Latency GET");

  const hasAny = latSet || latGet;

  if (!hasAny) {
    return (
      <section className={styles.panel} aria-labelledby="latency-title">
        <h3 id="latency-title" className={styles.title}>Latency</h3>
        <p className={styles.empty}>
          Latency data not found in benchmark results.
          Run without <code>--no-latency</code> to include it.
        </p>
      </section>
    );
  }

  return (
    <section className={styles.panel} aria-labelledby="latency-title">
      <div className={styles.header}>
        <h3 id="latency-title" className={styles.title}>
          Latency (per-operation)
        </h3>
        <span className={styles.badge}>Benchmark · single-threaded · includes timer overhead</span>
      </div>

      <div className={styles.tableWrapper}>
        <table className={styles.table} aria-label="Per-operation latency benchmarks">
          <thead>
            <tr>
              <th scope="col" className={styles.thLabel}>Operation</th>
              <th scope="col" className={styles.thNum}>avg</th>
              <th scope="col" className={styles.thNum}>p50</th>
              <th scope="col" className={styles.thNum}>p95</th>
              <th scope="col" className={styles.thNum}>p99</th>
            </tr>
          </thead>
          <tbody>
            {latSet && (
              <tr>
                <td className={styles.tdLabel}>SET</td>
                <td className={styles.tdNum}>{formatLatencyUs(latSet.latency_us.avg)}</td>
                <td className={styles.tdNum}>{formatLatencyUs(latSet.latency_us.p50)}</td>
                <td className={styles.tdNum}>{formatLatencyUs(latSet.latency_us.p95)}</td>
                <td className={`${styles.tdNum} ${styles.p99}`}>{formatLatencyUs(latSet.latency_us.p99)}</td>
              </tr>
            )}
            {latGet && (
              <tr>
                <td className={styles.tdLabel}>GET</td>
                <td className={styles.tdNum}>{formatLatencyUs(latGet.latency_us.avg)}</td>
                <td className={styles.tdNum}>{formatLatencyUs(latGet.latency_us.p50)}</td>
                <td className={styles.tdNum}>{formatLatencyUs(latGet.latency_us.p95)}</td>
                <td className={`${styles.tdNum} ${styles.p99}`}>{formatLatencyUs(latGet.latency_us.p99)}</td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      <p className={styles.note}>
        Latency is measured with per-operation timing. The{" "}
        <code>std::chrono::steady_clock</code> overhead (~nanoseconds) is
        included in each sample. Units: µs = microseconds, ms = milliseconds.
      </p>
    </section>
  );
}
