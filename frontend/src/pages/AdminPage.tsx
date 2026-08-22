import { Empty } from "../components/Empty";
import styles from "./Page.module.css";

export function AdminPage() {
  return (
    <div className={styles.page}>
      <header className={styles.pageHeader}>
        <h1 className={styles.pageTitle}>Admin</h1>
        <p className={styles.pageDescription}>
          Advanced operations: compaction, snapshots, and diagnostics.
        </p>
      </header>

      <div className={styles.pageBody}>
        <Empty
          icon="⊛"
          title="Admin panel coming in a future stage"
          description="WAL compaction, snapshot management, and engine diagnostics will be available here."
        />
      </div>
    </div>
  );
}
