// =============================================================================
// MaintenancePanel — Operational maintenance controls (Stage 17)
//
// Provides buttons for:
//   • Create Snapshot  → POST /snapshot
//   • Compact WAL      → POST /compact  (requires confirmation)
//
// UX behaviour:
//   - Snapshot button: protected by a simple confirmation dialog (it writes
//     to disk and may momentarily hold the write lock).
//   - Compact button: requires explicit confirmation; displays a warning
//     that it rewrites the WAL and may block writes briefly.
//   - Both buttons are disabled while the operation is in-flight, preventing
//     accidental double-triggers.
//   - On success: show success toast + call onSuccess() so the parent can
//     refresh /stats.
//   - On failure: show error toast; page remains functional.
//
// Accessibility:
//   - Confirmation dialogs use role="dialog" with aria-modal and aria-labelledby.
//   - Buttons have clear aria-labels.
//   - Keyboard: Escape key closes the confirmation dialog.
//   - Focus is moved into the dialog on open.
// =============================================================================

import { useCallback, useEffect, useRef, useState } from "react";
import { postCompact, postSnapshot } from "../services/api";
import type { ToastKind } from "./Toast";
import styles from "./MaintenancePanel.module.css";

interface MaintenancePanelProps {
  /** Called after a successful snapshot or compact so the parent can refresh stats. */
  onSuccess: () => void;
  /** Push a toast to the page-level toast stack. */
  addToast: (text: string, kind: ToastKind) => void;
}

type ConfirmTarget = "snapshot" | "compact" | null;

export function MaintenancePanel({ onSuccess, addToast }: MaintenancePanelProps) {
  const [inFlight, setInFlight] = useState<"snapshot" | "compact" | null>(null);
  const [confirmTarget, setConfirmTarget] = useState<ConfirmTarget>(null);

  // Focus management for the confirmation dialog
  const dialogRef = useRef<HTMLDivElement>(null);
  const cancelBtnRef = useRef<HTMLButtonElement>(null);

  // Move focus into dialog when it opens
  useEffect(() => {
    if (confirmTarget !== null) {
      // Small delay to ensure the dialog is rendered
      const t = setTimeout(() => cancelBtnRef.current?.focus(), 50);
      return () => clearTimeout(t);
    }
  }, [confirmTarget]);

  // Close dialog on Escape
  useEffect(() => {
    if (confirmTarget === null) return;
    const handler = (e: KeyboardEvent) => {
      if (e.key === "Escape") setConfirmTarget(null);
    };
    document.addEventListener("keydown", handler);
    return () => document.removeEventListener("keydown", handler);
  }, [confirmTarget]);

  const handleConfirm = useCallback(async () => {
    if (!confirmTarget) return;
    const target = confirmTarget;
    setConfirmTarget(null);
    setInFlight(target);

    try {
      if (target === "snapshot") {
        const result = await postSnapshot();
        if (result.ok) {
          addToast("Snapshot created successfully.", "success");
          onSuccess();
        } else {
          addToast(`Snapshot failed: ${result.error}`, "error");
        }
      } else {
        const result = await postCompact();
        if (result.ok) {
          addToast("WAL compacted successfully.", "success");
          onSuccess();
        } else {
          addToast(`Compaction failed: ${result.error}`, "error");
        }
      }
    } catch {
      addToast(`Unexpected error during ${target}.`, "error");
    } finally {
      setInFlight(null);
    }
  }, [confirmTarget, addToast, onSuccess]);

  const isDisabled = inFlight !== null;

  return (
    <section className={styles.panel} aria-labelledby="maintenance-heading">
      <h2 id="maintenance-heading" className={styles.heading}>
        Maintenance
      </h2>

      <p className={styles.description}>
        Manual operational controls. These operations affect persistence state
        and may briefly hold an exclusive lock on the storage engine.
      </p>

      <div className={styles.actions}>
        {/* ---- Create Snapshot ---- */}
        <div className={styles.action}>
          <div className={styles.actionInfo}>
            <span className={styles.actionIcon} aria-hidden="true">📸</span>
            <div>
              <p className={styles.actionLabel}>Create Snapshot</p>
              <p className={styles.actionHint}>
                Writes the full in-memory state to a snapshot file. Safe and
                idempotent — previous snapshot is replaced.
              </p>
            </div>
          </div>
          <button
            type="button"
            className={`${styles.btn} ${styles.btnPrimary}`}
            disabled={isDisabled}
            aria-label="Create a snapshot of the current store state"
            onClick={() => setConfirmTarget("snapshot")}
          >
            {inFlight === "snapshot" ? (
              <>
                <span className={styles.spinner} aria-hidden="true" />
                Snapshotting…
              </>
            ) : (
              "Create Snapshot"
            )}
          </button>
        </div>

        {/* ---- Compact WAL ---- */}
        <div className={styles.action}>
          <div className={styles.actionInfo}>
            <span className={styles.actionIcon} aria-hidden="true">🗜</span>
            <div>
              <p className={styles.actionLabel}>Compact WAL</p>
              <p className={styles.actionHint}>
                Rewrites the WAL to contain only the current live state, removing
                obsolete entries and reclaiming disk space. May briefly block
                write operations.
              </p>
            </div>
          </div>
          <button
            type="button"
            className={`${styles.btn} ${styles.btnWarning}`}
            disabled={isDisabled}
            aria-label="Compact the Write-Ahead Log"
            onClick={() => setConfirmTarget("compact")}
          >
            {inFlight === "compact" ? (
              <>
                <span className={styles.spinner} aria-hidden="true" />
                Compacting…
              </>
            ) : (
              "Compact WAL"
            )}
          </button>
        </div>
      </div>

      {/* ---- Confirmation Dialog ---- */}
      {confirmTarget !== null && (
        <div
          className={styles.backdrop}
          aria-hidden="true"
          onClick={() => setConfirmTarget(null)}
        />
      )}
      {confirmTarget !== null && (
        <div
          ref={dialogRef}
          role="dialog"
          aria-modal="true"
          aria-labelledby="confirm-dialog-title"
          className={styles.dialog}
        >
          <h3 id="confirm-dialog-title" className={styles.dialogTitle}>
            {confirmTarget === "snapshot" ? "Create Snapshot?" : "Compact WAL?"}
          </h3>
          <p className={styles.dialogBody}>
            {confirmTarget === "snapshot"
              ? "This will write the current in-memory state to disk as a snapshot file. The previous snapshot will be replaced."
              : "This will rewrite the WAL to contain only the current live state. Obsolete entries will be removed. This may briefly block write operations while the WAL is rewritten."}
          </p>
          <div className={styles.dialogActions}>
            <button
              ref={cancelBtnRef}
              type="button"
              className={`${styles.btn} ${styles.btnGhost}`}
              onClick={() => setConfirmTarget(null)}
            >
              Cancel
            </button>
            <button
              type="button"
              className={`${styles.btn} ${confirmTarget === "compact" ? styles.btnWarning : styles.btnPrimary}`}
              onClick={handleConfirm}
              aria-label={`Confirm: ${confirmTarget === "snapshot" ? "create snapshot" : "compact WAL"}`}
            >
              {confirmTarget === "snapshot" ? "Snapshot" : "Compact"}
            </button>
          </div>
        </div>
      )}
    </section>
  );
}
