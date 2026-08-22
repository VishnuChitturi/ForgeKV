// =============================================================================
// SnapshotCompactionPanel — Stage 18 Benchmark Metrics
//
// Displays benchmark results for the Snapshot and Compaction workloads.
// These are time-to-complete measurements, not throughput workloads.
//
// Data source: BENCHMARK ARTIFACT (benchmark-results.json)
// Workload names: "Snapshot", "Compaction"
// Fields: elapsed_s, ops (key count), wal_before_bytes, wal_after_bytes
//
// These are benchmark measurements, NOT live operation times.
// =============================================================================

import type { BenchmarkResults } from "../types/benchmark";
import {
  findWorkload,
  formatElapsed,
  formatWalReduction,
} from "../utils/benchmark";
import { formatBytes, formatNumber } from "../utils/format";
import styles from "./SnapshotCompactionPanel.module.css";

interface SnapshotCompactionPanelProps {
  data: BenchmarkResults;
}

export function SnapshotCompactionPanel({ data }: SnapshotCompactionPanelProps) {
  const snap = findWorkload(data.workloads, "Snapshot");
  const compact = findWorkload(data.workloads, "Compaction");

  if (!snap && !compact) {
    return (
      <section className={styles.panel} aria-labelledby="snap-compact-title">
        <h3 id="snap-compact-title" className={styles.title}>
          Snapshot &amp; Compaction
        </h3>
        <p className={styles.empty}>No snapshot or compaction benchmark data found.</p>
      </section>
    );
  }

  const compactionReduction =
    compact
      ? formatWalReduction(compact.wal_before_bytes, compact.wal_after_bytes)
      : "—";

  return (
    <section className={styles.panel} aria-labelledby="snap-compact-title">
      <div className={styles.header}>
        <h3 id="snap-compact-title" className={styles.title}>
          Snapshot &amp; Compaction
        </h3>
        <span className={styles.badge}>Benchmark measurements · not throughput</span>
      </div>

      <div className={styles.cards}>
        {/* Snapshot card */}
        {snap && (
          <div className={styles.card} aria-label="Snapshot benchmark">
            <div className={styles.cardHeader}>
              <span className={styles.cardIcon} aria-hidden="true">📸</span>
              <h4 className={styles.cardTitle}>Snapshot</h4>
            </div>
            <dl className={styles.rows}>
              <div className={styles.row}>
                <dt>Duration</dt>
                <dd className={styles.highlight}>{formatElapsed(snap.elapsed_s)}</dd>
              </div>
              <div className={styles.row}>
                <dt>Dataset</dt>
                <dd>{formatNumber(snap.ops)} keys</dd>
              </div>
              <div className={styles.row}>
                <dt>WAL before</dt>
                <dd>{formatBytes(snap.wal_before_bytes)}</dd>
              </div>
              <div className={styles.row}>
                <dt>WAL after snapshot</dt>
                <dd>{formatBytes(snap.wal_after_bytes)}</dd>
              </div>
            </dl>
            <p className={styles.note}>
              Full in-memory state written to disk as a checkpoint.
            </p>
          </div>
        )}

        {/* Compaction card */}
        {compact && (
          <div className={styles.card} aria-label="Compaction benchmark">
            <div className={styles.cardHeader}>
              <span className={styles.cardIcon} aria-hidden="true">🗜️</span>
              <h4 className={styles.cardTitle}>Compaction</h4>
            </div>
            <dl className={styles.rows}>
              <div className={styles.row}>
                <dt>Duration</dt>
                <dd className={styles.highlight}>{formatElapsed(compact.elapsed_s)}</dd>
              </div>
              <div className={styles.row}>
                <dt>WAL before</dt>
                <dd>{formatBytes(compact.wal_before_bytes)}</dd>
              </div>
              <div className={styles.row}>
                <dt>WAL after</dt>
                <dd>{formatBytes(compact.wal_after_bytes)}</dd>
              </div>
              <div className={styles.row}>
                <dt>WAL reduction</dt>
                <dd className={styles.highlight}>{compactionReduction}</dd>
              </div>
              <div className={styles.row}>
                <dt>Key count</dt>
                <dd>{formatNumber(compact.ops)}</dd>
              </div>
            </dl>
            <p className={styles.note}>
              WAL rewritten to contain only live state, removing stale entries.
            </p>
          </div>
        )}
      </div>
    </section>
  );
}
