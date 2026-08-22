import { useEffect, useId, useRef } from "react";
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

// Selectors for all naturally focusable elements inside a container.
const FOCUSABLE =
  'a[href], button:not([disabled]), textarea:not([disabled]), ' +
  'input:not([disabled]), select:not([disabled]), ' +
  '[tabindex]:not([tabindex="-1"])';

/**
 * Modal — accessible dialog overlay.
 *
 * Stage 19 changes:
 *   - Focus trap: Tab/Shift+Tab cycle only within the dialog.
 *   - aria-modal="true" moved to the panel div (the actual dialog element).
 *   - role="dialog" moved to the panel div.
 *   - Overlay is role="presentation" so assistive tech sees only the dialog.
 *   - Unique title ID per instance via useId() to avoid duplicate ids when
 *     multiple modals mount simultaneously (e.g. nested confirm dialogs).
 */
export function Modal({
  isOpen,
  onClose,
  title,
  children,
  panelClass,
}: ModalProps) {
  const panelRef = useRef<HTMLDivElement>(null);
  const titleId  = useId();

  // -------------------------------------------------------------------------
  // Keyboard: Escape closes; Tab cycles focus within the dialog.
  // -------------------------------------------------------------------------
  useEffect(() => {
    if (!isOpen) return;

    const handleKeyDown = (e: KeyboardEvent) => {
      if (e.key === "Escape") {
        onClose();
        return;
      }

      if (e.key !== "Tab") return;

      const panel = panelRef.current;
      if (!panel) return;

      const focusable = Array.from(panel.querySelectorAll<HTMLElement>(FOCUSABLE));
      if (focusable.length === 0) return;

      const first = focusable[0];
      const last  = focusable[focusable.length - 1];

      if (e.shiftKey) {
        // Shift+Tab from first → wrap to last
        if (document.activeElement === first) {
          e.preventDefault();
          last.focus();
        }
      } else {
        // Tab from last → wrap to first
        if (document.activeElement === last) {
          e.preventDefault();
          first.focus();
        }
      }
    };

    document.addEventListener("keydown", handleKeyDown);
    return () => document.removeEventListener("keydown", handleKeyDown);
  }, [isOpen, onClose]);

  // -------------------------------------------------------------------------
  // Focus the first focusable element (or the panel itself) when opening.
  // -------------------------------------------------------------------------
  useEffect(() => {
    if (!isOpen) return;

    // Small delay so the DOM is painted before we attempt to focus.
    const id = requestAnimationFrame(() => {
      const panel = panelRef.current;
      if (!panel) return;
      const first = panel.querySelector<HTMLElement>(FOCUSABLE);
      if (first) {
        first.focus();
      } else {
        panel.focus();
      }
    });
    return () => cancelAnimationFrame(id);
  }, [isOpen]);

  if (!isOpen) return null;

  return (
    // Overlay — purely visual. role="presentation" means AT ignores it.
    <div
      className={styles.overlay}
      role="presentation"
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      {/* Dialog panel — aria-modal and role="dialog" belong here. */}
      <div
        className={`${styles.panel} ${panelClass ?? ""}`}
        ref={panelRef}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        tabIndex={-1}
      >
        <div className={styles.header}>
          <h2 id={titleId} className={styles.title}>
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
