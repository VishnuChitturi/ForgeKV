import styles from "./ErrorMessage.module.css";

interface ErrorMessageProps {
  title?: string;
  message: string;
  /** Optional retry callback. */
  onRetry?: () => void;
}

export function ErrorMessage({
  title = "Something went wrong",
  message,
  onRetry,
}: ErrorMessageProps) {
  return (
    <div className={styles.wrapper} role="alert">
      <div className={styles.icon} aria-hidden="true">⚠</div>
      <div className={styles.body}>
        <p className={styles.title}>{title}</p>
        <p className={styles.message}>{message}</p>
        {onRetry && (
          <button className={styles.retry} onClick={onRetry} type="button">
            Try again
          </button>
        )}
      </div>
    </div>
  );
}
