// =============================================================================
// ForgeKV — formatting helpers
//
// Pure, side-effect-free utility functions for displaying server metrics in a
// human-readable form on the dashboard.
// =============================================================================

/**
 * formatUptime — convert a number of seconds into a concise duration string.
 *
 * Examples:
 *   0       → "0s"
 *   12      → "12s"
 *   134     → "2m 14s"
 *   4320    → "1h 12m"
 *   187200  → "2d 4h"
 */
export function formatUptime(seconds: number): string {
  const s = Math.floor(seconds);
  if (s < 60) return `${s}s`;

  const mins  = Math.floor(s / 60);
  const secs  = s % 60;
  if (mins < 60) return secs > 0 ? `${mins}m ${secs}s` : `${mins}m`;

  const hours = Math.floor(mins / 60);
  const remMins = mins % 60;
  if (hours < 24) return remMins > 0 ? `${hours}h ${remMins}m` : `${hours}h`;

  const days = Math.floor(hours / 24);
  const remHours = hours % 24;
  return remHours > 0 ? `${days}d ${remHours}h` : `${days}d`;
}

/**
 * formatBytes — display a byte count with appropriate units.
 *
 * Examples:
 *   0       → "0 B"
 *   512     → "512 B"
 *   1229    → "1.2 KB"
 *   3565158 → "3.4 MB"
 *   1181116006 → "1.1 GB"
 */
export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
}

/**
 * formatNumber — format an integer with locale-aware thousand separators.
 *
 * Examples:
 *   12431    → "12,431"
 *   1234567  → "1,234,567"
 */
export function formatNumber(n: number): string {
  return n.toLocaleString("en-US");
}

/**
 * formatSnapshotTime — convert a microsecond Unix timestamp to a human-readable
 * local date/time string.
 *
 * - 0 (never snapshotted) → "Never"
 * - otherwise → locale-formatted date + time, e.g. "Aug 22, 2026, 14:03"
 *
 * IMPORTANT: last_snapshot_time_us is microseconds since Unix epoch.
 * JavaScript Date expects milliseconds, so divide by 1000.
 */
export function formatSnapshotTime(us: number): string {
  if (us === 0) return "Never";
  const ms = us / 1000;
  return new Date(ms).toLocaleString("en-US", {
    year:   "numeric",
    month:  "short",
    day:    "numeric",
    hour:   "2-digit",
    minute: "2-digit",
  });
}

/**
 * formatLastUpdated — human-readable relative "last updated" description.
 *
 * This is a frontend-only timestamp (Date.now()) — it has nothing to do with
 * last_snapshot_time_us.
 */
export function formatLastUpdated(ts: number | null): string {
  if (ts === null) return "Never";
  const diffMs = Date.now() - ts;
  if (diffMs < 5_000)  return "just now";
  if (diffMs < 60_000) return `${Math.floor(diffMs / 1000)}s ago`;
  if (diffMs < 3_600_000) return `${Math.floor(diffMs / 60_000)}m ago`;
  return new Date(ts).toLocaleTimeString("en-US", { hour: "2-digit", minute: "2-digit" });
}

/**
 * formatTtl — human-readable TTL for a key.
 *
 * ttl_seconds semantics from the backend:
 *   -1   → permanent (no TTL)
 *   >= 0 → remaining seconds
 *
 * Examples:
 *   -1     → "Permanent"
 *   0      → "Expiring…"
 *   45     → "45s"
 *   134    → "2m 14s"
 *   3601   → "1h 0m"
 */
export function formatTtl(ttl_seconds: number): string {
  if (ttl_seconds < 0) return "Permanent";
  if (ttl_seconds < 1) return "Expiring…";
  return formatUptime(ttl_seconds);
}

/**
 * truncateValue — shorten a long value string for table display.
 *
 * Returns at most maxLen visible characters, appending "…" if truncated.
 * Newlines are replaced with a visible marker so the table stays single-line.
 * The original value is NEVER modified.
 */
export function truncateValue(value: string, maxLen = 80): string {
  // Replace newlines with a visible marker for single-line display.
  const oneLine = value.replace(/\n/g, "↵").replace(/\r/g, "");
  if (oneLine.length <= maxLen) return oneLine;
  return oneLine.slice(0, maxLen) + "…";
}
