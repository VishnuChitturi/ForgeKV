// =============================================================================
// AnalyticsOverview — Stage 18 Live Analytics section
//
// Groups the live analytics components:
//   - HitRateCard       (GET hit rate with donut chart)
//   - OperationBreakdown (cumulative op counters with CSS bars)
//
// Data source: LIVE SERVER METRICS (GET /stats)
// =============================================================================

import type { StatsResponse } from "../types/api";
import { HitRateCard } from "./HitRateCard";
import { OperationBreakdown } from "./OperationBreakdown";
import styles from "./AnalyticsOverview.module.css";

interface AnalyticsOverviewProps {
  stats: StatsResponse;
}

export function AnalyticsOverview({ stats }: AnalyticsOverviewProps) {
  return (
    <div className={styles.grid}>
      <HitRateCard stats={stats} />
      <OperationBreakdown stats={stats} />
    </div>
  );
}
