// =============================================================================
// PersistenceAnalytics — Stage 18 Live Persistence Metrics
//
// Shows persistence-related fields from GET /stats with richer context than
// the Stage 17 PersistencePanel.  Includes TTL and expiry analytics.
//
// Data source: LIVE SERVER METRICS (GET /stats)
// Fields: key_count, wal_size_bytes, last_snapshot_time_us,
//         expired_count, ttl_set_count
//
// Does NOT invent disk IOPS, memory usage, or filesystem throughput.
// Only what the backend actually exposes is shown.
// =============================================================================

import type { StatsResponse } from "../types/api";
import { formatBytes, formatNumber, formatSnapshotTime } from "../utils/format";
import styles from "./PersistenceAnalytics.module.css";

interface PersistenceAnalyticsProps {
  stats: StatsResponse;
}

export function PersistenceAnalytics({ stats }: PersistenceAnalyticsProps) {
  // TTL key expiry ratio: expired / ttl_set (rough expiry rate indicator)
  const ttlTotal = stats.ttl_set_count;
  const expiredPct =
    ttlTotal > 0
      ? ((stats.expired_count / ttlTotal) * 100).toFixed(1) + "%"
      : "—";

  const snapshotText = formatSnapshotTime(stats.last_snapshot_time_us);
  const snapshotAge = stats.last_snapshot_time_us === 0 ? null : snapshotAgo(stats.last_snapshot_time_us);

  return (
    <section className={styles.panel} aria-labelledby="persist-analytics-title">
      <div className={styles.header}>
        <h3 id="persist-analytics-title" className={styles.title}>
          Persistence &amp; Storage
        </h3>
        <span className={styles.badge}>Live server metric</span>
      </div>

      <div className={styles.grid}>
        {/* WAL */}
        <div className={styles.metricBlock}>
          <span className={styles.metricIcon} aria-hidden="true">📄</span>
          <div className={styles.metricBody}>
            <span className={styles.metricLabel}>WAL Size</span>
            <span className={styles.metricValue}>{formatBytes(stats.wal_size_bytes)}</span>
            <span className={styles.metricSub}>Write-Ahead Log on disk</span>
          </div>
        </div>

        {/* Live keys */}
        <div className={styles.metricBlock}>
          <span className={styles.metricIcon} aria-hidden="true">🔑</span>
          <div className={styles.metricBody}>
            <span className={styles.metricLabel}>Live Keys</span>
            <span className={styles.metricValue}>{formatNumber(stats.key_count)}</span>
            <span className={styles.metricSub}>Currently in the engine</span>
          </div>
        </div>

        {/* Last snapshot */}
        <div className={styles.metricBlock}>
          <span className={styles.metricIcon} aria-hidden="true">📸</span>
          <div className={styles.metricBody}>
            <span className={styles.metricLabel}>Last Snapshot</span>
            <span className={styles.metricValue}>{snapshotAge ?? snapshotText}</span>
            {snapshotAge && (
              <span className={styles.metricSub}>{snapshotText}</span>
            )}
            {stats.last_snapshot_time_us === 0 && (
              <span className={styles.metricSub}>No snapshot taken yet</span>
            )}
          </div>
        </div>

        {/* Expired keys */}
        <div className={styles.metricBlock}>
          <span className={styles.metricIcon} aria-hidden="true">⏱️</span>
          <div className={styles.metricBody}>
            <span className={styles.metricLabel}>Expired Keys</span>
            <span className={styles.metricValue}>{formatNumber(stats.expired_count)}</span>
            <span className={styles.metricSub}>Since server start</span>
          </div>
        </div>
      </div>

      {/* TTL analytics row */}
      {ttlTotal > 0 && (
        <div className={styles.ttlRow}>
          <span className={styles.ttlLabel}>TTL Analytics</span>
          <span className={styles.ttlDetail}>
            {formatNumber(stats.ttl_set_count)} keys set with TTL ·{" "}
            {formatNumber(stats.expired_count)} expired ·{" "}
            {expiredPct} expiry rate
          </span>
        </div>
      )}
    </section>
  );
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Format a microsecond UTC timestamp as a relative "X ago" string. */
function snapshotAgo(us: number): string {
  const diffMs = Date.now() - us / 1000;
  if (diffMs < 0) return "just now";
  const diffSec = Math.floor(diffMs / 1000);
  if (diffSec < 60) return `${diffSec}s ago`;
  const diffMin = Math.floor(diffSec / 60);
  if (diffMin < 60) return `${diffMin}m ago`;
  const diffHr = Math.floor(diffMin / 60);
  if (diffHr < 24) return `${diffHr}h ago`;
  return `${Math.floor(diffHr / 24)}d ago`;
}
