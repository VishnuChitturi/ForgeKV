// =============================================================================
// HitRateCard — Stage 18 Live Analytics
//
// Displays the GET hit rate as a large percentage figure alongside a
// CSS-based donut chart.  All data comes from the live GET /stats response.
//
// Data source: LIVE SERVER METRICS (GET /stats)
// Fields used: get_hits, get_misses
// Formula: hits / (hits + misses) * 100
// Edge case: if hits + misses === 0, shows "N/A" not "100%"
// =============================================================================

import type { StatsResponse } from "../types/api";
import { formatNumber } from "../utils/format";
import styles from "./HitRateCard.module.css";

interface HitRateCardProps {
  stats: StatsResponse;
}

export function HitRateCard({ stats }: HitRateCardProps) {
  const total = stats.get_hits + stats.get_misses;
  const hasData = total > 0;
  const hitPct = hasData ? (stats.get_hits / total) * 100 : null;
  const displayPct = hitPct !== null ? `${hitPct.toFixed(1)}%` : "N/A";

  // SVG donut parameters
  const radius = 36;
  const stroke = 7;
  const cx = 46;
  const cy = 46;
  const circumference = 2 * Math.PI * radius;
  // Fraction of donut that represents hits (or full grey if no data)
  const hitFraction = hitPct !== null ? hitPct / 100 : 0;
  const hitDash = hitFraction * circumference;
  const missDash = circumference - hitDash;

  // Accent colour depends on hit rate
  let accentClass = styles.accentNeutral;
  if (hitPct !== null) {
    if (hitPct >= 90) accentClass = styles.accentGood;
    else if (hitPct >= 60) accentClass = styles.accentWarn;
    else accentClass = styles.accentBad;
  }

  return (
    <article className={styles.card} aria-label={`GET hit rate: ${displayPct}`}>
      {/* Donut chart */}
      <div className={styles.donutWrapper} aria-hidden="true">
        <svg
          width="92"
          height="92"
          viewBox="0 0 92 92"
          className={styles.donut}
          role="img"
          aria-label={`Donut chart: ${displayPct} hit rate`}
        >
          {/* Background track */}
          <circle
            cx={cx}
            cy={cy}
            r={radius}
            fill="none"
            stroke="var(--color-surface-subtle)"
            strokeWidth={stroke}
          />
          {hasData && hitPct !== null && hitPct > 0 && (
            <circle
              cx={cx}
              cy={cy}
              r={radius}
              fill="none"
              className={accentClass}
              strokeWidth={stroke}
              strokeDasharray={`${hitDash} ${missDash}`}
              // Start at top (−90°)
              strokeDashoffset={circumference / 4}
              strokeLinecap="round"
            />
          )}
        </svg>
        <span className={`${styles.donutLabel} ${accentClass}`} aria-hidden="true">
          {displayPct}
        </span>
      </div>

      {/* Text content */}
      <div className={styles.body}>
        <h3 className={styles.title}>GET Hit Rate</h3>
        <p className={styles.badge}>Live server metric</p>

        {hasData ? (
          <dl className={styles.stats}>
            <div className={styles.statRow}>
              <dt className={styles.statLabel}>Hits</dt>
              <dd className={`${styles.statValue} ${styles.hitColor}`}>
                {formatNumber(stats.get_hits)}
              </dd>
            </div>
            <div className={styles.statRow}>
              <dt className={styles.statLabel}>Misses</dt>
              <dd className={`${styles.statValue} ${styles.missColor}`}>
                {formatNumber(stats.get_misses)}
              </dd>
            </div>
            <div className={styles.statRow}>
              <dt className={styles.statLabel}>Total GETs</dt>
              <dd className={styles.statValue}>{formatNumber(total)}</dd>
            </div>
          </dl>
        ) : (
          <p className={styles.noData}>
            No GET operations recorded yet.
          </p>
        )}
      </div>
    </article>
  );
}
