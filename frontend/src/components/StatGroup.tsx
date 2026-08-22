// =============================================================================
// StatGroup — a titled section containing a list of label/value stat rows.
//
// Stage 19: Changed heading from h2 to h3. StatGroup is always rendered
// inside a section that already has an h2 heading, so using h2 here creates
// a duplicate same-level heading and breaks the document outline.
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
      <h3 className={styles.title}>{title}</h3>
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
