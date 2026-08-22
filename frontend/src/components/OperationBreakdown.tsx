// =============================================================================
// OperationBreakdown — Stage 18 Live Analytics
//
// Visualizes the distribution of operations since server start using
// horizontal CSS bar chart.
//
// Data source: LIVE SERVER METRICS (GET /stats)
// Fields: get_hits, get_misses, set_count, delete_count, ttl_set_count, expired_count
//
// Important: these are CUMULATIVE COUNTERS since process start.
// They are NOT requests per second or any rate metric.
// =============================================================================

import type { StatsResponse } from "../types/api";
import { formatNumber } from "../utils/format";
import styles from "./OperationBreakdown.module.css";

interface OperationBreakdownProps {
  stats: StatsResponse;
}

interface OpRow {
  label: string;
  value: number;
  colorClass: string;
}

export function OperationBreakdown({ stats }: OperationBreakdownProps) {
  const rows: OpRow[] = [
    { label: "GET (hit)",   value: stats.get_hits,      colorClass: styles.barHit },
    { label: "GET (miss)",  value: stats.get_misses,    colorClass: styles.barMiss },
    { label: "SET",         value: stats.set_count,     colorClass: styles.barSet },
    { label: "DELETE",      value: stats.delete_count,  colorClass: styles.barDelete },
    { label: "TTL SET",     value: stats.ttl_set_count, colorClass: styles.barTtl },
    { label: "Expired",     value: stats.expired_count, colorClass: styles.barExpired },
  ];

  const maxVal = Math.max(...rows.map((r) => r.value), 1);
  const total = rows.reduce((sum, r) => sum + r.value, 0);

  return (
    <section className={styles.panel} aria-labelledby="op-breakdown-title">
      <div className={styles.header}>
        <h3 id="op-breakdown-title" className={styles.title}>
          Operation Breakdown
        </h3>
        <span className={styles.badge}>Live server metric · cumulative since start</span>
      </div>

      {total === 0 ? (
        <p className={styles.empty}>No operations recorded yet.</p>
      ) : (
        <div className={styles.chart} role="list" aria-label="Operation counts">
          {rows.map(({ label, value, colorClass }) => {
            const widthPct = (value / maxVal) * 100;
            const pctOfTotal =
              total > 0 ? ((value / total) * 100).toFixed(1) : "0.0";

            return (
              <div
                key={label}
                className={styles.row}
                role="listitem"
                aria-label={`${label}: ${formatNumber(value)} operations (${pctOfTotal}%)`}
              >
                <span className={styles.rowLabel}>{label}</span>
                <div className={styles.barTrack} aria-hidden="true">
                  <div
                    className={`${styles.bar} ${colorClass}`}
                    style={{ width: `${widthPct}%` }}
                  />
                </div>
                <span className={styles.rowValue}>{formatNumber(value)}</span>
                <span className={styles.rowPct}>{pctOfTotal}%</span>
              </div>
            );
          })}
        </div>
      )}

      <p className={styles.footnote}>
        Total operations recorded: {formatNumber(total)}
      </p>
    </section>
  );
}
