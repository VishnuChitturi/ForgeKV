// =============================================================================
// ConcurrencyPanel — Stage 18 Benchmark Metrics
//
// Displays concurrent GET / SET / Mixed benchmark results showing how
// throughput scales with thread count.
//
// Data source: BENCHMARK ARTIFACT (benchmark-results.json)
// Workload names used:
//   "Concurrent GET"    (threads = 1, 2, 4, 8)
//   "Concurrent SET"    (threads = 1, 2, 4, 8)
//   "Concurrent Mixed"  (threads = 1, 2, 4, 8)
//
// These are benchmark measurements, NOT live concurrency metrics.
// =============================================================================

import type { BenchmarkResults, BenchWorkload } from "../types/benchmark";
import { findWorkloads, formatOpsPerSec } from "../utils/benchmark";
import styles from "./ConcurrencyPanel.module.css";

interface ConcurrencyPanelProps {
  data: BenchmarkResults;
}

interface ConcurrencyGroup {
  label: string;
  fragment: string;
}

const GROUPS: ConcurrencyGroup[] = [
  { label: "Concurrent GET",   fragment: "Concurrent GET" },
  { label: "Concurrent SET",   fragment: "Concurrent SET" },
  { label: "Concurrent Mixed", fragment: "Concurrent Mixed" },
];

export function ConcurrencyPanel({ data }: ConcurrencyPanelProps) {
  // Build groups: each group is an array of results sorted by thread count
  const groups = GROUPS.map(({ label, fragment }) => {
    const workloads = findWorkloads(data.workloads, fragment)
      .slice()
      .sort((a, b) => a.threads - b.threads);
    return { label, workloads };
  });

  const hasAny = groups.some((g) => g.workloads.length > 0);

  if (!hasAny) {
    return (
      <section className={styles.panel} aria-labelledby="concurrency-title">
        <h3 id="concurrency-title" className={styles.title}>Concurrency Scaling</h3>
        <p className={styles.empty}>No concurrency benchmark data found.</p>
      </section>
    );
  }

  return (
    <section className={styles.panel} aria-labelledby="concurrency-title">
      <div className={styles.header}>
        <h3 id="concurrency-title" className={styles.title}>
          Concurrency Scaling
        </h3>
        <span className={styles.badge}>Benchmark · std::shared_mutex engine</span>
      </div>

      <div className={styles.groups}>
        {groups.map(({ label, workloads }) => (
          <ConcurrencyGroup key={label} label={label} workloads={workloads} />
        ))}
      </div>
    </section>
  );
}

// ---------------------------------------------------------------------------
// Inner: one concurrency group (GET / SET / Mixed)
// ---------------------------------------------------------------------------

function ConcurrencyGroup({
  label,
  workloads,
}: {
  label: string;
  workloads: BenchWorkload[];
}) {
  if (workloads.length === 0) {
    return (
      <div className={styles.group}>
        <h4 className={styles.groupTitle}>{label}</h4>
        <p className={styles.empty}>No data</p>
      </div>
    );
  }

  const maxOps = Math.max(...workloads.map((w) => w.ops_per_sec), 1);

  return (
    <div className={styles.group}>
      <h4 className={styles.groupTitle}>{label}</h4>
      <div className={styles.tableWrapper}>
        <table
          className={styles.table}
          aria-label={`${label} concurrency scaling`}
        >
          <thead>
            <tr>
              <th scope="col" className={styles.thThreads}>Threads</th>
              <th scope="col" className={styles.thBar}>Throughput</th>
              <th scope="col" className={styles.thNum}>Ops/sec</th>
            </tr>
          </thead>
          <tbody>
            {workloads.map((w) => {
              const barPct = (w.ops_per_sec / maxOps) * 100;
              return (
                <tr key={w.threads}>
                  <td className={styles.tdThreads}>{w.threads}</td>
                  <td className={styles.tdBar} aria-hidden="true">
                    <div className={styles.barTrack}>
                      <div
                        className={styles.bar}
                        style={{ width: `${barPct}%` }}
                      />
                    </div>
                  </td>
                  <td className={styles.tdNum} aria-label={`${w.threads} threads: ${formatOpsPerSec(w.ops_per_sec)} ops/sec`}>
                    {formatOpsPerSec(w.ops_per_sec)}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      </div>
    </div>
  );
}
