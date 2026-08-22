// =============================================================================
// SystemInfo — System information section of the Admin Dashboard (Stage 17)
//
// Displays information that is actually available and reliable:
//   • Backend version — from CMakeLists.txt (0.13.0), hard-coded as a
//     compile-time/project constant. Exposing it via HTTP would require an
//     additional /version endpoint with no other operational value; skipped
//     per the Stage 17 scope. Displayed as a known project constant.
//   • Frontend version — from package.json (0.1.0).
//   • HTTP server status — derived from the live /health result.
//   • Persistence model — a fixed description of ForgeKV's durability strategy,
//     which is stable and factually accurate for this project version.
//
// Nothing is fabricated. CPU/memory/disk/network metrics are NOT shown because
// the backend does not provide them.
// =============================================================================

import type { ServerStatus } from "../hooks/useServerStatus";
import styles from "./SystemInfo.module.css";

// These values are accurate project constants documented in CMakeLists.txt
// and package.json respectively. They do not require runtime fetching.
const BACKEND_VERSION  = "0.13.0";
const FRONTEND_VERSION = "0.1.0";

interface SystemInfoProps {
  serverStatus: ServerStatus;
}

const STATUS_LABEL: Record<ServerStatus, string> = {
  loading: "Checking…",
  connected: "Reachable",
  offline: "Unreachable",
};

export function SystemInfo({ serverStatus }: SystemInfoProps) {
  return (
    <section className={styles.panel} aria-labelledby="sysinfo-heading">
      <h2 id="sysinfo-heading" className={styles.heading}>
        System Information
      </h2>

      <dl className={styles.infoList}>
        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>Backend version</dt>
          <dd className={styles.infoValue}>
            <code className={styles.code}>v{BACKEND_VERSION}</code>
          </dd>
        </div>

        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>Frontend version</dt>
          <dd className={styles.infoValue}>
            <code className={styles.code}>v{FRONTEND_VERSION}</code>
          </dd>
        </div>

        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>HTTP server</dt>
          <dd className={styles.infoValue}>
            <span
              className={`${styles.statusChip} ${styles[serverStatus]}`}
              aria-label={`HTTP server: ${STATUS_LABEL[serverStatus]}`}
            >
              {STATUS_LABEL[serverStatus]}
            </span>
          </dd>
        </div>

        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>Persistence model</dt>
          <dd className={styles.infoValue}>
            Binary WAL + Snapshots
          </dd>
        </div>

        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>Concurrency model</dt>
          <dd className={styles.infoValue}>
            <code className={styles.code}>std::shared_mutex</code>
          </dd>
        </div>

        <div className={styles.infoRow}>
          <dt className={styles.infoLabel}>HTTP library</dt>
          <dd className={styles.infoValue}>
            cpp-httplib v0.18.5
          </dd>
        </div>
      </dl>

      <p className={styles.note}>
        CPU, memory, disk, and network metrics are not available from the
        ForgeKV backend. Only data the server actually provides is shown.
      </p>
    </section>
  );
}
