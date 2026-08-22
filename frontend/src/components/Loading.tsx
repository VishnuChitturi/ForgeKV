import styles from "./Loading.module.css";

interface LoadingProps {
  /** Accessible label shown to screen readers. */
  label?: string;
  /** Size: 'sm' | 'md' (default) | 'lg' */
  size?: "sm" | "md" | "lg";
}

export function Loading({ label = "Loading…", size = "md" }: LoadingProps) {
  return (
    <div
      className={`${styles.wrapper} ${styles[size]}`}
      role="status"
      aria-label={label}
    >
      <span className={styles.spinner} aria-hidden="true" />
      <span className={styles.label}>{label}</span>
    </div>
  );
}
