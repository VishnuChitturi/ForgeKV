// =============================================================================
// PersistencePanel — Persistence information section of the Admin Dashboard
//
// Shows persistence-relevant information available from GET /stats:
//   WAL size, Last snapshot, Key count, Expired key count
//
// All values come from the real /stats endpoint.
// No invented metrics. Only fields the backend actually provides are shown.
//
// WAL design notes (displayed to inform operators):
//   - ForgeKV uses an append-only binary WAL (Write-Ahead Log).
//   - Each mutation is written to the WAL before the in-memory state changes.
//   - The WAL grows with every write; compact() rewrites it to reclaim space.
//   - Snapshots checkpoint the full in-memory state to disk independently
//     of the WAL. On restart, the snapshot is loaded first, then any
//     subsequent WAL records are replayed.
// =============================================================================

import { StatGroup } from "./StatGroup";
import type { StatsResponse } from "../types/api";
import {
  formatBytes,
  formatNumber,
  formatSnapshotTime,
} from "../utils/format";

interface PersistencePanelProps {
  stats: StatsResponse;
}

export function PersistencePanel({ stats }: PersistencePanelProps) {
  return (
    <StatGroup
      title="Persistence"
      rows={[
        { label: "WAL size",          value: formatBytes(stats.wal_size_bytes) },
        { label: "Last snapshot",     value: formatSnapshotTime(stats.last_snapshot_time_us) },
        { label: "Live key count",    value: formatNumber(stats.key_count) },
        { label: "Expired key count", value: formatNumber(stats.expired_count) },
      ]}
    />
  );
}
