// =============================================================================
// OperationStats — Operational counters section of the Admin Dashboard
//
// Displays all six operation counters from GET /stats:
//   GET hits, GET misses, SET operations, DELETE operations,
//   TTL sets, Expired keys
//
// Uses the existing StatGroup component for consistent presentation.
// No charts — Stage 18 handles visualization.
// All data comes from the real /stats endpoint. No fake metrics.
// =============================================================================

import { StatGroup } from "./StatGroup";
import type { StatsResponse } from "../types/api";
import { formatNumber } from "../utils/format";

interface OperationStatsProps {
  stats: StatsResponse;
}

export function OperationStats({ stats }: OperationStatsProps) {
  const hitRate =
    stats.get_hits + stats.get_misses > 0
      ? ((stats.get_hits / (stats.get_hits + stats.get_misses)) * 100).toFixed(1) + "%"
      : "—";

  return (
    <StatGroup
      title="Operation Statistics"
      rows={[
        { label: "GET hits",         value: formatNumber(stats.get_hits) },
        { label: "GET misses",       value: formatNumber(stats.get_misses) },
        { label: "Hit rate",         value: hitRate },
        { label: "SET operations",   value: formatNumber(stats.set_count) },
        { label: "DELETE operations",value: formatNumber(stats.delete_count) },
        { label: "TTL sets",         value: formatNumber(stats.ttl_set_count) },
        { label: "Expired keys",     value: formatNumber(stats.expired_count) },
      ]}
    />
  );
}
