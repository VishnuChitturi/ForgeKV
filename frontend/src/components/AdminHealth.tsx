// =============================================================================
// AdminHealth — Server health section of the Admin Dashboard (Stage 17)
//
// Displays operational health state from GET /health + GET /stats:
//   • Server status (Online / Offline)
//   • Uptime
//   • Live key count
//   • WAL size
//   • Last snapshot time
//
// Emphasises operational state, not raw metric counts. Those belong in
// OperationStats. Does not duplicate the full Stage 15 dashboard.
// =============================================================================

import type { ServerStatus } from "../hooks/useServerStatus";
import type { StatsResponse } from "../types/api";
import {
  formatBytes,
  formatNumber,
  formatSnapshotTime,
  formatUptime,
} from "../utils/format";
import styles from "./AdminHealth.module.css";

interface AdminHealthProps {
  serverStatus: ServerStatus;
  stats: StatsResponse;
}

const STATUS_LABEL: Record<ServerStatus, string> = {
  loading: "Checking…",
  connected: "Online",
  offline: "Offline",
};

export function AdminHealth({ serverStatus, stats }: AdminHealthProps) {
  return (
    <section className={styles.panel} aria-labelledby="admin-health-heading">
      <h2 id="admin-health-heading" className={styles.heading}>
        Server Health
      </h2>

      {/* Status row */}
      <div className={styles.statusRow}>
        <span
          className={`${styles.statusDot} ${styles[serverStatus]}`}
          aria-hidden="true"
        />
        <span
          className={`${styles.statusLabel} ${styles[`text_${serverStatus}`]}`}
          aria-label={`Server status: ${STATUS_LABEL[serverStatus]}`}
        >
          {STATUS_LABEL[serverStatus]}
        </span>
      </div>

      {/* Key metrics as a definition list */}
      <dl className={styles.metricList}>
        <div className={styles.metricRow}>
          <dt className={styles.metricLabel}>
            <span aria-hidden="true">⏱</span> Uptime
          </dt>
          <dd className={styles.metricValue}>
            {formatUptime(stats.uptime_seconds)}
          </dd>
        </div>

        <div className={styles.metricRow}>
          <dt className={styles.metricLabel}>
            <span aria-hidden="true">🗝</span> Live Keys
          </dt>
          <dd className={styles.metricValue}>
            {formatNumber(stats.key_count)}
          </dd>
        </div>

        <div className={styles.metricRow}>
          <dt className={styles.metricLabel}>
            <span aria-hidden="true">💾</span> WAL Size
          </dt>
          <dd className={styles.metricValue}>
            {formatBytes(stats.wal_size_bytes)}
          </dd>
        </div>

        <div className={styles.metricRow}>
          <dt className={styles.metricLabel}>
            <span aria-hidden="true">📸</span> Last Snapshot
          </dt>
          <dd className={styles.metricValue}>
            {formatSnapshotTime(stats.last_snapshot_time_us)}
          </dd>
        </div>
      </dl>
    </section>
  );
}
