// =============================================================================
// ServerOverview — displays server health + key metrics in a horizontal row
//
// Shows: Server Status · Uptime · Key Count · WAL Size · Last Snapshot
// =============================================================================
import type { ServerStatus } from "../hooks/useServerStatus";
import type { StatsResponse } from "../types/api";
import {
  formatBytes,
  formatNumber,
  formatSnapshotTime,
  formatUptime,
} from "../utils/format";
import styles from "./ServerOverview.module.css";

interface ServerOverviewProps {
  serverStatus: ServerStatus;
  stats: StatsResponse;
}

const STATUS_LABEL: Record<ServerStatus, string> = {
  loading: "Checking…",
  connected: "Online",
  offline:   "Offline",
};

export function ServerOverview({ serverStatus, stats }: ServerOverviewProps) {
  return (
    <section className={styles.overview} aria-label="Server overview">
      {/* Status pill */}
      <div className={`${styles.item} ${styles.statusItem}`}>
        <span
          className={`${styles.statusDot} ${styles[serverStatus]}`}
          aria-hidden="true"
        />
        <div className={styles.itemText}>
          <span className={styles.itemLabel}>Server</span>
          <span
            className={`${styles.itemValue} ${styles[`statusText_${serverStatus}`]}`}
            aria-label={`Server status: ${STATUS_LABEL[serverStatus]}`}
          >
            {STATUS_LABEL[serverStatus]}
          </span>
        </div>
      </div>

      <div className={styles.divider} aria-hidden="true" />

      {/* Uptime */}
      <div className={styles.item}>
        <span className={styles.itemIcon} aria-hidden="true">⏱</span>
        <div className={styles.itemText}>
          <span className={styles.itemLabel}>Uptime</span>
          <span className={styles.itemValue}>{formatUptime(stats.uptime_seconds)}</span>
        </div>
      </div>

      <div className={styles.divider} aria-hidden="true" />

      {/* Key count */}
      <div className={styles.item}>
        <span className={styles.itemIcon} aria-hidden="true">🗝</span>
        <div className={styles.itemText}>
          <span className={styles.itemLabel}>Keys</span>
          <span className={styles.itemValue}>{formatNumber(stats.key_count)}</span>
        </div>
      </div>

      <div className={styles.divider} aria-hidden="true" />

      {/* WAL size */}
      <div className={styles.item}>
        <span className={styles.itemIcon} aria-hidden="true">💾</span>
        <div className={styles.itemText}>
          <span className={styles.itemLabel}>WAL Size</span>
          <span className={styles.itemValue}>{formatBytes(stats.wal_size_bytes)}</span>
        </div>
      </div>

      <div className={styles.divider} aria-hidden="true" />

      {/* Last snapshot */}
      <div className={styles.item}>
        <span className={styles.itemIcon} aria-hidden="true">📸</span>
        <div className={styles.itemText}>
          <span className={styles.itemLabel}>Last Snapshot</span>
          <span className={styles.itemValue}>
            {formatSnapshotTime(stats.last_snapshot_time_us)}
          </span>
        </div>
      </div>
    </section>
  );
}
