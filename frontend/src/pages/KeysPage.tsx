import { Empty } from "../components/Empty";
import styles from "./Page.module.css";

export function KeysPage() {
  return (
    <div className={styles.page}>
      <header className={styles.pageHeader}>
        <h1 className={styles.pageTitle}>Keys</h1>
        <p className={styles.pageDescription}>
          Browse, create, update, and delete keys in the ForgeKV store.
        </p>
      </header>

      <div className={styles.pageBody}>
        <Empty
          icon="⊟"
          title="Key browser coming in a future stage"
          description="You'll be able to search keys, inspect values, set TTLs, and delete entries from here."
        />
      </div>
    </div>
  );
}
