// =============================================================================
// StatGroup — a titled section containing a list of label/value stat rows.
//
// Used for the secondary "Operation Statistics" and "Persistence" sections
// on the dashboard. Keeps DashboardPage.tsx free of repetitive markup.
// =============================================================================
import styles from "./StatGroup.module.css";

export interface StatRow {
  label: string;
  value: string;
}

interface StatGroupProps {
  title: string;
  rows: StatRow[];
}

export function StatGroup({ title, rows }: StatGroupProps) {
  return (
    <section className={styles.group} aria-label={title}>
      <h2 className={styles.title}>{title}</h2>
      <dl className={styles.list}>
        {rows.map(({ label, value }) => (
          <div className={styles.row} key={label}>
            <dt className={styles.label}>{label}</dt>
            <dd className={styles.value}>{value}</dd>
          </div>
        ))}
      </dl>
    </section>
  );
}
