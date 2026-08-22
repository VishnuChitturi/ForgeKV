// =============================================================================
// AdminPage — ForgeKV Admin Dashboard (Stage 17)
//
// Operational control and monitoring page for the ForgeKV engine.
//
// Answers the questions an operator would ask:
//   "Is the server healthy?"
//   "What is its current state?"
//   "How is persistence doing?"
//   "What operational actions are available?"
//
// Layout:
//   1. Page header  — title, description, last-updated, refresh button
//   2. AdminHealth  — server status + key operational metrics
//   3. Detail row   — OperationStats + PersistencePanel side by side
//   4. Maintenance  — Create Snapshot + Compact WAL controls
//   5. SystemInfo   — project version, HTTP library, concurrency model
//
// Refresh:
//   Manual-only. GET /health + GET /stats run in parallel.
//   Last-updated timestamp is a frontend timestamp (Date.now()).
//   No automatic polling. No websockets.
//
// Error handling:
//   - First load: shows Loading spinner, then ErrorMessage on failure.
//   - Subsequent refreshes: shows subtle opacity change while loading;
//     keeps last known data visible on error via toast.
//   - Backend offline is surfaced in AdminHealth (status: Offline) and
//     handled gracefully — the page does not crash.
//   - Snapshot/compact failures surface as error toasts; the page stays
//     functional.
// =============================================================================

import { useCallback, useEffect, useRef, useState } from "react";
import { AdminHealth } from "../components/AdminHealth";
import { ErrorMessage } from "../components/ErrorMessage";
import { Loading } from "../components/Loading";
import { MaintenancePanel } from "../components/MaintenancePanel";
import { OperationStats } from "../components/OperationStats";
import { PersistencePanel } from "../components/PersistencePanel";
import { SystemInfo } from "../components/SystemInfo";
import { Toast, useToast } from "../components/Toast";
import type { ServerStatus } from "../hooks/useServerStatus";
import { getHealth, getStats } from "../services/api";
import type { StatsResponse } from "../types/api";
import { formatLastUpdated } from "../utils/format";
import styles from "./AdminPage.module.css";
import pageStyles from "./Page.module.css";

// ---------------------------------------------------------------------------
// Page state
// ---------------------------------------------------------------------------
type AdminState =
  | { phase: "loading" }
  | { phase: "error"; message: string }
  | { phase: "ready"; stats: StatsResponse; serverStatus: ServerStatus };

export function AdminPage() {
  const [state, setState] = useState<AdminState>({ phase: "loading" });
  const [refreshing, setRefreshing] = useState(false);
  const [lastUpdated, setLastUpdated] = useState<number | null>(null);

  // Controls whether we show a full loading overlay vs subtle refresh state.
  const hasLoadedRef = useRef(false);

  const { toasts, addToast, dismissToast } = useToast();

  // -------------------------------------------------------------------------
  // Fetch /health + /stats in parallel
  // -------------------------------------------------------------------------
  const fetchData = useCallback(async () => {
    if (hasLoadedRef.current) {
      setRefreshing(true);
    } else {
      setState({ phase: "loading" });
    }

    const [healthResult, statsResult] = await Promise.all([
      getHealth(),
      getStats(),
    ]);

    setRefreshing(false);

    if (!statsResult.ok) {
      if (hasLoadedRef.current) {
        // Keep the current data visible; notify via toast.
        addToast(
          `Refresh failed: ${statsResult.error || "Unable to reach server."}`,
          "error"
        );
      } else {
        setState({
          phase: "error",
          message: statsResult.error || "Unable to load ForgeKV statistics.",
        });
      }
      return;
    }

    const serverStatus: ServerStatus = healthResult.ok ? "connected" : "offline";
    hasLoadedRef.current = true;
    setState({ phase: "ready", stats: statsResult.data, serverStatus });
    setLastUpdated(Date.now());
  }, [addToast]);

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  // -------------------------------------------------------------------------
  // Render
  // -------------------------------------------------------------------------
  return (
    <div className={pageStyles.page}>
      {/* ── Page header ── */}
      <header className={pageStyles.pageHeader}>
        <div className={styles.headerRow}>
          <div>
            <h1 className={pageStyles.pageTitle}>Admin Dashboard</h1>
            <p className={pageStyles.pageDescription}>
              Operational control and monitoring for the ForgeKV engine.
            </p>
          </div>
          <div className={styles.headerActions}>
            {lastUpdated !== null && (
              <span className={styles.lastUpdated} aria-live="polite">
                Updated: {formatLastUpdated(lastUpdated)}
              </span>
            )}
            <button
              className={styles.refreshBtn}
              type="button"
              onClick={fetchData}
              disabled={refreshing || state.phase === "loading"}
              aria-label="Refresh admin dashboard"
            >
              <span
                className={refreshing ? styles.refreshIconSpin : styles.refreshIcon}
                aria-hidden="true"
              >
                ↻
              </span>
              {refreshing ? "Refreshing…" : "Refresh"}
            </button>
          </div>
        </div>
      </header>

      {/* ── Page body ── */}
      <div className={pageStyles.pageBody}>
        {/* Initial load spinner */}
        {state.phase === "loading" && (
          <div className={styles.loadingCenter}>
            <Loading label="Loading admin dashboard…" size="lg" />
          </div>
        )}

        {/* Error on first load */}
        {state.phase === "error" && (
          <div className={styles.errorWrapper}>
            <ErrorMessage
              title="Unable to load ForgeKV statistics."
              message={state.message}
              onRetry={fetchData}
            />
          </div>
        )}

        {/* Ready state */}
        {state.phase === "ready" && (
          <div
            className={`${styles.content} ${refreshing ? styles.contentRefreshing : ""}`}
          >
            {/* ── Section: Server Health ── */}
            <section aria-labelledby="section-health">
              <h2 id="section-health" className={styles.sectionTitle}>
                Server Health
              </h2>
              <AdminHealth
                serverStatus={state.serverStatus}
                stats={state.stats}
              />
            </section>

            {/* ── Section: Storage & Persistence ── */}
            <section aria-labelledby="section-storage">
              <h2 id="section-storage" className={styles.sectionTitle}>
                Storage &amp; Persistence
              </h2>
              <div className={styles.detailGrid}>
                <OperationStats stats={state.stats} />
                <PersistencePanel stats={state.stats} />
              </div>
            </section>

            {/* ── Section: Maintenance ── */}
            <section aria-labelledby="section-maintenance">
              <h2 id="section-maintenance" className={styles.sectionTitle}>
                Maintenance
              </h2>
              <MaintenancePanel
                onSuccess={fetchData}
                addToast={addToast}
              />
            </section>

            {/* ── Section: System Information ── */}
            <section aria-labelledby="section-sysinfo">
              <h2 id="section-sysinfo" className={styles.sectionTitle}>
                System Information
              </h2>
              <SystemInfo serverStatus={state.serverStatus} />
            </section>
          </div>
        )}
      </div>

      {/* ── Toast notifications ── */}
      <Toast toasts={toasts} onDismiss={dismissToast} />
    </div>
  );
}
