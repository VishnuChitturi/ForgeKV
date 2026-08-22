// =============================================================================
// BenchmarkOverview — Stage 18 Benchmark Metrics
//
// Shows throughput (ops/sec) for the core sequential KV workloads:
//   Sequential SET, GET (hit), GET (miss), DELETE, Mixed, TTL SET
//
// Data source: BENCHMARK ARTIFACT (benchmark-results.json)
// These are controlled benchmark measurements, NOT live server metrics.
// =============================================================================

import type { BenchmarkResults } from "../types/benchmark";
import {
  findWorkload,
  formatOpsPerSec,
  formatElapsed,
} from "../utils/benchmark";
import styles from "./BenchmarkOverview.module.css";

interface BenchmarkOverviewProps {
  data: BenchmarkResults;
}

interface WorkloadRow {
  label: string;
  name: string; // matches BenchWorkload.name from the JSON
}

const WORKLOAD_ROWS: WorkloadRow[] = [
  { label: "Sequential SET",    name: "Sequential SET" },
  { label: "GET (hit)",          name: "Sequential GET (hit)" },
  { label: "GET (miss)",         name: "Sequential GET (miss)" },
  { label: "DELETE",             name: "Sequential DELETE" },
  { label: "Mixed (50G/30S/10D/10M)", name: "Mixed (50G/30S/10D/10M)" },
  { label: "TTL SET",            name: "TTL SET (set_with_ttl)" },
];

export function BenchmarkOverview({ data }: BenchmarkOverviewProps) {
  const cfg = data.config;

  return (
    <section className={styles.panel} aria-labelledby="bench-overview-title">
      <div className={styles.header}>
        <h3 id="bench-overview-title" className={styles.title}>
          KV Engine Throughput
        </h3>
        <span className={styles.meta}>
          {cfg.operations.toLocaleString("en-US")} ops · {cfg.value_size_bytes}B values
        </span>
      </div>

      <div className={styles.tableWrapper}>
        <table className={styles.table} aria-label="KV engine throughput benchmarks">
          <thead>
            <tr>
              <th scope="col" className={styles.thLabel}>Workload</th>
              <th scope="col" className={styles.thNum}>Ops/sec</th>
              <th scope="col" className={styles.thNum}>Duration</th>
              <th scope="col" className={styles.thNum}>Operations</th>
            </tr>
          </thead>
          <tbody>
            {WORKLOAD_ROWS.map(({ label, name }) => {
              const w = findWorkload(data.workloads, name);
              if (!w) {
                return (
                  <tr key={name} className={styles.rowMissing}>
                    <td className={styles.tdLabel}>{label}</td>
                    <td colSpan={3} className={styles.tdMissing}>
                      Not in benchmark results
                    </td>
                  </tr>
                );
              }
              return (
                <tr key={name}>
                  <td className={styles.tdLabel}>{label}</td>
                  <td className={styles.tdNum} aria-label={`${label}: ${formatOpsPerSec(w.ops_per_sec)} ops per second`}>
                    <span className={styles.opsValue}>{formatOpsPerSec(w.ops_per_sec)}</span>
                    <span className={styles.opsUnit}> ops/sec</span>
                  </td>
                  <td className={styles.tdNum}>{formatElapsed(w.elapsed_s)}</td>
                  <td className={styles.tdNum}>{w.ops.toLocaleString("en-US")}</td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </section>
  );
}
