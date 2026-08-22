// =============================================================================
// BenchmarkNotice — Stage 18
//
// Prominent notice shown at the top of the Benchmark Performance section.
// Clarifies that all values in this section are controlled benchmark
// measurements, NOT live server metrics.
//
// Also shown as a graceful empty state when no benchmark results are available.
// =============================================================================

import styles from "./BenchmarkNotice.module.css";

interface BenchmarkNoticeProps {
  /** When true, shows the "unavailable" state instead of the normal notice. */
  unavailable?: boolean;
  /** Reason text when unavailable */
  reason?: string;
  /** ISO-8601 timestamp from the benchmark artifact */
  generatedAt?: string;
  /** ForgeKV version the benchmark was run against */
  forgekvVersion?: string;
}

export function BenchmarkNotice({
  unavailable = false,
  reason,
  generatedAt,
  forgekvVersion,
}: BenchmarkNoticeProps) {
  if (unavailable) {
    return (
      <div className={styles.unavailable} role="status">
        <span className={styles.unavailableIcon} aria-hidden="true">📊</span>
        <div className={styles.unavailableBody}>
          <strong className={styles.unavailableTitle}>
            Benchmark data unavailable
          </strong>
          {reason && <p className={styles.unavailableReason}>{reason}</p>}
          <p className={styles.unavailableHint}>
            Generate benchmark results by running:
          </p>
          <code className={styles.unavailableCmd}>
            ./build/forgekv_benchmark --json-output frontend/public/benchmark-results.json
          </code>
          <p className={styles.unavailableHint}>
            Then restart the dev server (or rebuild the frontend).
          </p>
        </div>
      </div>
    );
  }

  return (
    <div className={styles.notice} role="note">
      <span className={styles.noticeIcon} aria-hidden="true">ℹ️</span>
      <div className={styles.noticeBody}>
        <strong>Benchmark Performance</strong> — these are controlled benchmark
        measurements, not live production metrics. They were measured in an
        isolated environment and represent the performance characteristics of
        the ForgeKV storage engine under synthetic load.
        {forgekvVersion && (
          <span className={styles.meta}> · ForgeKV {forgekvVersion}</span>
        )}
        {generatedAt && (
          <span className={styles.meta}>
            {" "}
            · Generated {formatGeneratedAt(generatedAt)}
          </span>
        )}
      </div>
    </div>
  );
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

function formatGeneratedAt(iso: string): string {
  try {
    return new Date(iso).toLocaleString("en-US", {
      year: "numeric",
      month: "short",
      day: "numeric",
      hour: "2-digit",
      minute: "2-digit",
      timeZoneName: "short",
    });
  } catch {
    return iso;
  }
}
