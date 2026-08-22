import { useCallback, useEffect, useState } from "react";
import styles from "./Toast.module.css";

export type ToastKind = "success" | "error" | "info";

export interface ToastMessage {
  id: number;
  kind: ToastKind;
  text: string;
}

interface ToastProps {
  toasts: ToastMessage[];
  onDismiss: (id: number) => void;
}

/**
 * Toast — lightweight notification stack.
 *
 * Renders a stack of auto-dismissing toasts in the bottom-right corner.
 * Each toast auto-dismisses after 3 seconds. Users can also dismiss manually.
 */
export function Toast({ toasts, onDismiss }: ToastProps) {
  return (
    <div className={styles.container} aria-live="polite" aria-atomic="false">
      {toasts.map((t) => (
        <ToastItem key={t.id} toast={t} onDismiss={onDismiss} />
      ))}
    </div>
  );
}

function ToastItem({
  toast,
  onDismiss,
}: {
  toast: ToastMessage;
  onDismiss: (id: number) => void;
}) {
  useEffect(() => {
    const timer = setTimeout(() => onDismiss(toast.id), 3000);
    return () => clearTimeout(timer);
  }, [toast.id, onDismiss]);

  const icon =
    toast.kind === "success" ? "✓" : toast.kind === "error" ? "✕" : "ℹ";

  return (
    <div
      className={`${styles.toast} ${styles[toast.kind]}`}
      role="status"
      aria-label={toast.text}
    >
      <span className={styles.icon} aria-hidden="true">
        {icon}
      </span>
      <span className={styles.text}>{toast.text}</span>
      <button
        className={styles.dismiss}
        type="button"
        onClick={() => onDismiss(toast.id)}
        aria-label="Dismiss notification"
      >
        ×
      </button>
    </div>
  );
}

// ---------------------------------------------------------------------------
// useToast hook — manage toast state in a parent component
// ---------------------------------------------------------------------------

let _nextId = 1;

export function useToast() {
  const [toasts, setToasts] = useState<ToastMessage[]>([]);

  const addToast = useCallback((text: string, kind: ToastKind = "info") => {
    const id = _nextId++;
    setToasts((prev) => [...prev, { id, kind, text }]);
  }, []);

  const dismissToast = useCallback((id: number) => {
    setToasts((prev) => prev.filter((t) => t.id !== id));
  }, []);

  return { toasts, addToast, dismissToast };
}
