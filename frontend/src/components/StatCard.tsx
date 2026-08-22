// =============================================================================
// StatCard — a single summary metric card on the dashboard
//
// Props:
//   label   — short human-readable name (e.g. "GET Hits")
//   value   — already-formatted display string (e.g. "12,431")
//   icon    — emoji / unicode glyph displayed at the top-left of the card
//   accent  — optional: 'ok' | 'error' | 'warning' for coloured accents
// =============================================================================
import styles from "./StatCard.module.css";

export type StatCardAccent = "ok" | "error" | "warning" | "neutral";

interface StatCardProps {
  label: string;
  value: string;
  icon: string;
  accent?: StatCardAccent;
  /** Optional sub-label shown below the value in smaller muted text. */
  sublabel?: string;
}

export function StatCard({
  label,
  value,
  icon,
  accent = "neutral",
  sublabel,
}: StatCardProps) {
  return (
    <article className={`${styles.card} ${styles[accent]}`} aria-label={`${label}: ${value}`}>
      <span className={styles.icon} aria-hidden="true">{icon}</span>
      <div className={styles.body}>
        <span className={styles.label}>{label}</span>
        <span className={styles.value}>{value}</span>
        {sublabel && <span className={styles.sublabel}>{sublabel}</span>}
      </div>
    </article>
  );
}
