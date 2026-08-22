import type { ServerStatus } from "../hooks/useServerStatus";
import styles from "./ServerStatus.module.css";

interface ServerStatusProps {
  status: ServerStatus;
  onRefresh?: () => void;
}

const STATUS_LABEL: Record<ServerStatus, string> = {
  loading: "Checking…",
  connected: "Connected",
  offline: "Offline",
};

export function ServerStatusBadge({ status, onRefresh }: ServerStatusProps) {
  return (
    <button
      className={`${styles.badge} ${styles[status]}`}
      onClick={onRefresh}
      title={
        onRefresh ? "Click to re-check server connection" : undefined
      }
      type="button"
      aria-label={`Server status: ${STATUS_LABEL[status]}`}
    >
      <span className={styles.dot} aria-hidden="true" />
      <span className={styles.label}>
        Server&nbsp;&nbsp;{STATUS_LABEL[status]}
      </span>
    </button>
  );
}
