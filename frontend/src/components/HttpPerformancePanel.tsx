// =============================================================================
// HttpPerformancePanel — Stage 18 Benchmark Metrics
//
// Displays HTTP-level benchmark results for GET, PUT, and Mixed workloads
// across different thread counts.
//
// Data source: BENCHMARK ARTIFACT (benchmark-results.json)
// Workload name fragments used:
//   "HTTP GET (hit)"
//   "HTTP PUT"
//   "HTTP Mixed"
//
// These are benchmark measurements, NOT live HTTP request rates.
// HTTP throughput is fundamentally limited by the network stack overhead
// even on loopback (127.0.0.1).
// =============================================================================

import type { BenchmarkResults } from "../types/benchmark";
import { findWorkloads, formatOpsPerSec, formatElapsed } from "../utils/benchmark";
import styles from "./HttpPerformancePanel.module.css";

interface HttpPerformancePanelProps {
  data: BenchmarkResults;
}

interface HttpGroup {
  label: string;
  fragment: string;
}

const GROUPS: HttpGroup[] = [
  { label: "HTTP GET",   fragment: "HTTP GET (hit)" },
  { label: "HTTP PUT",   fragment: "HTTP PUT" },
  { label: "HTTP Mixed", fragment: "HTTP Mixed" },
];

export function HttpPerformancePanel({ data }: HttpPerformancePanelProps) {
  const groups = GROUPS.map(({ label, fragment }) => {
    const workloads = findWorkloads(data.workloads, fragment)
      .slice()
      .sort((a, b) => a.threads - b.threads);
    return { label, workloads };
  });

  const hasAny = groups.some((g) => g.workloads.length > 0);

  if (!hasAny || !data.config.http_enabled) {
    return (
      <section className={styles.panel} aria-labelledby="http-perf-title">
        <h3 id="http-perf-title" className={styles.title}>HTTP Performance</h3>
        <p className={styles.empty}>
          {!data.config.http_enabled
            ? "HTTP benchmarks were skipped (--no-http)."
            : "No HTTP benchmark data found in results."}
        </p>
      </section>
    );
  }

  return (
    <section className={styles.panel} aria-labelledby="http-perf-title">
      <div className={styles.header}>
        <h3 id="http-perf-title" className={styles.title}>
          HTTP Performance
        </h3>
        <span className={styles.badge}>Benchmark · loopback (127.0.0.1) · network-stack limited</span>
      </div>

      <div className={styles.tableWrapper}>
        <table className={styles.table} aria-label="HTTP performance benchmarks">
          <thead>
            <tr>
              <th scope="col" className={styles.thLabel}>Workload</th>
              <th scope="col" className={styles.thNum}>Threads</th>
              <th scope="col" className={styles.thNum}>Ops/sec</th>
              <th scope="col" className={styles.thNum}>Duration</th>
              <th scope="col" className={styles.thNum}>Ops</th>
            </tr>
          </thead>
          <tbody>
            {groups.map(({ label, workloads }) =>
              workloads.length === 0 ? (
                <tr key={label} className={styles.rowMissing}>
                  <td className={styles.tdLabel}>{label}</td>
                  <td colSpan={4} className={styles.tdMissing}>
                    Not in results
                  </td>
                </tr>
              ) : (
                workloads.map((w, i) => (
                  <tr key={`${label}-${w.threads}`}>
                    <td className={styles.tdLabel}>
                      {i === 0 ? label : ""}
                    </td>
                    <td className={styles.tdNum}>{w.threads}</td>
                    <td className={styles.tdNum} aria-label={`${label} ${w.threads} threads: ${formatOpsPerSec(w.ops_per_sec)} ops/sec`}>
                      {formatOpsPerSec(w.ops_per_sec)}
                    </td>
                    <td className={styles.tdNum}>{formatElapsed(w.elapsed_s)}</td>
                    <td className={styles.tdNum}>{w.ops.toLocaleString("en-US")}</td>
                  </tr>
                ))
              )
            )}
          </tbody>
        </table>
      </div>

      <p className={styles.note}>
        HTTP throughput reflects real loopback TCP round-trips through the
        cpp-httplib server. It is always lower than raw in-process KV
        throughput due to network-stack overhead.
      </p>
    </section>
  );
}
