import { Empty } from "../components/Empty";
import styles from "./Page.module.css";

export function DashboardPage() {
  return (
    <div className={styles.page}>
      <header className={styles.pageHeader}>
        <h1 className={styles.pageTitle}>Dashboard</h1>
        <p className={styles.pageDescription}>
          Engine metrics and operational overview.
        </p>
      </header>

      <div className={styles.pageBody}>
        <Empty
          icon="⊞"
          title="Dashboard coming in Stage 15"
          description="Key counts, operation rates, WAL size, uptime, and snapshot status will be displayed here."
        />
      </div>
    </div>
  );
}
