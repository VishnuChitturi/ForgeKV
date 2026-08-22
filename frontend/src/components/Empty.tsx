import styles from "./Empty.module.css";

interface EmptyProps {
  icon?: string;
  title: string;
  description?: string;
  action?: React.ReactNode;
}

export function Empty({ icon = "○", title, description, action }: EmptyProps) {
  return (
    <div className={styles.wrapper}>
      <span className={styles.icon} aria-hidden="true">{icon}</span>
      <p className={styles.title}>{title}</p>
      {description && <p className={styles.description}>{description}</p>}
      {action && <div className={styles.action}>{action}</div>}
    </div>
  );
}
