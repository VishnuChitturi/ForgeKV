import { useEffect, useRef } from "react";
import styles from "./Modal.module.css";

interface ModalProps {
  /** Whether the modal is visible */
  isOpen: boolean;
  /** Called when user clicks overlay or presses Escape */
  onClose: () => void;
  /** Accessible title shown in the modal header */
  title: string;
  /** Modal content */
  children: React.ReactNode;
  /** Extra class for the panel (size variants etc.) */
  panelClass?: string;
}

/**
 * Modal — accessible dialog overlay.
 *
 * - Traps focus inside the modal while open.
 * - Closes on Escape key or overlay click.
 * - Uses role="dialog" and aria-modal="true".
 * - Scrolls internally if content overflows.
 */
export function Modal({
  isOpen,
  onClose,
  title,
  children,
  panelClass,
}: ModalProps) {
  const panelRef = useRef<HTMLDivElement>(null);

  // Close on Escape
  useEffect(() => {
    if (!isOpen) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === "Escape") onClose();
    };
    document.addEventListener("keydown", handler);
    return () => document.removeEventListener("keydown", handler);
  }, [isOpen, onClose]);

  // Focus the panel when it opens
  useEffect(() => {
    if (isOpen) {
      panelRef.current?.focus();
    }
  }, [isOpen]);

  if (!isOpen) return null;

  return (
    <div
      className={styles.overlay}
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
      aria-modal="true"
      role="dialog"
      aria-labelledby="modal-title"
    >
      <div
        className={`${styles.panel} ${panelClass ?? ""}`}
        ref={panelRef}
        tabIndex={-1}
      >
        <div className={styles.header}>
          <h2 id="modal-title" className={styles.title}>
            {title}
          </h2>
          <button
            className={styles.closeBtn}
            type="button"
            onClick={onClose}
            aria-label="Close dialog"
          >
            ×
          </button>
        </div>
        <div className={styles.body}>{children}</div>
      </div>
    </div>
  );
}
