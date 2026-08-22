// =============================================================================
// AdminPage — ForgeKV Admin Dashboard (Stage 18: Analytics + Performance)
//
// Extends the Stage 17 operator dashboard with analytics and performance
// visualization sections.
//
// SECTION LAYOUT:
//   1. Page header           — title, description, refresh button, last-updated
//   2. Server Health         — status + key operational metrics        [LIVE]
//   3. Analytics Overview    — GET hit rate donut + operation breakdown [LIVE]
//   4. Persistence Analytics — WAL, live keys, snapshot, TTL/expiry    [LIVE]
//   5. Storage & Persistence — OperationStats + PersistencePanel       [LIVE]
//   6. Maintenance           — Snapshot + Compact controls             [live action]
//   7. Benchmark Performance — throughput, latency, concurrency,
//                              HTTP, snapshot/compaction               [BENCHMARK ARTIFACT]
//   8. System Information    — project version, HTTP lib, model        [STATIC]
//
// DATA SOURCES:
//   LIVE  — GET /health + GET /stats, refreshed manually.
//   BENCH — /benchmark-results.json served from frontend/public/.
//            Loaded once on mount. NOT re-run on refresh.
//            Produced by: ./build/forgekv_benchmark --json-output
//                         frontend/public/benchmark-results.json
//
// These two data sources are ALWAYS clearly separated in the UI.
// Benchmark results are NEVER labeled as live metrics.
// =============================================================================

import { useCallback, useEffect, useRef, useState } from "react";

// Stage 17 components (preserved)
import { AdminHealth } from "../components/AdminHealth";
import { ErrorMessage } from "../components/ErrorMessage";
import { Loading } from "../components/Loading";
import { MaintenancePanel } from "../components/MaintenancePanel";
import { OperationStats } from "../components/OperationStats";
import { PersistencePanel } from "../components/PersistencePanel";
import { SystemInfo } from "../components/SystemInfo";
import { Toast, useToast } from "../components/Toast";

// Stage 18 — Live analytics components
import { AnalyticsOverview } from "../components/AnalyticsOverview";
import { PersistenceAnalytics } from "../components/PersistenceAnalytics";

// Stage 18 — Benchmark components
import { BenchmarkNotice } from "../components/BenchmarkNotice";
import { BenchmarkOverview } from "../components/BenchmarkOverview";
import { ConcurrencyPanel } from "../components/ConcurrencyPanel";
import { HttpPerformancePanel } from "../components/HttpPerformancePanel";
import { LatencyPanel } from "../components/LatencyPanel";
import { SnapshotCompactionPanel } from "../components/SnapshotCompactionPanel";

import type { ServerStatus } from "../hooks/useServerStatus";
import { getHealth, getStats } from "../services/api";
import type { StatsResponse } from "../types/api";
import type { BenchLoadState } from "../types/benchmark";
import { loadBenchmarkResults } from "../utils/benchmark";
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

  // Benchmark data is loaded once on mount. It is NOT re-fetched on refresh.
  const [benchState, setBenchState] = useState<BenchLoadState>({ status: "idle" });

  // Controls whether we show a full loading overlay vs subtle refresh state.
  const hasLoadedRef = useRef(false);

  const { toasts, addToast, dismissToast } = useToast();

  // -------------------------------------------------------------------------
  // Load benchmark artifact once on mount
  // -------------------------------------------------------------------------
  useEffect(() => {
    setBenchState({ status: "loading" });
    loadBenchmarkResults().then((result) => {
      if (result.ok) {
        setBenchState({ status: "ready", data: result.data });
      } else {
        setBenchState({ status: "unavailable", reason: result.reason });
      }
    });
  }, []); // empty deps — only runs once; benchmarks are not re-run on refresh

  // -------------------------------------------------------------------------
  // Fetch /health + /stats in parallel (live data only)
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
            <h1 className={pageStyles.pageTitle}>Analytics &amp; Performance</h1>
            <p className={pageStyles.pageDescription}>
              Live server metrics and benchmark performance for the ForgeKV engine.
            </p>
          </div>
          <div className={styles.headerActions}>
            {lastUpdated !== null && (
              <span className={styles.lastUpdated} aria-live="polite">
                Live data: {formatLastUpdated(lastUpdated)}
              </span>
            )}
            <button
              className={styles.refreshBtn}
              type="button"
              onClick={fetchData}
              disabled={refreshing || state.phase === "loading"}
              aria-label="Refresh live server metrics"
            >
              <span
                className={refreshing ? styles.refreshIconSpin : styles.refreshIcon}
                aria-hidden="true"
              >
                ↻
              </span>
              {refreshing ? "Refreshing…" : "Refresh Live"}
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
            {/* ── LIVE DATA SECTIONS ── */}

            {/* Section: Server Health */}
            <section aria-labelledby="section-health">
              <h2 id="section-health" className={styles.sectionTitle}>
                Server Health
              </h2>
              <AdminHealth
                serverStatus={state.serverStatus}
                stats={state.stats}
              />
            </section>

            {/* Section: Live Operation Analytics */}
            <section aria-labelledby="section-analytics">
              <h2 id="section-analytics" className={styles.sectionTitle}>
                Live Operation Analytics
              </h2>
              <p className={styles.sectionDesc}>
                Cumulative counters since server start — refreshed manually.
              </p>
              <AnalyticsOverview stats={state.stats} />
            </section>

            {/* Section: Persistence Analytics */}
            <section aria-labelledby="section-persist-analytics">
              <h2 id="section-persist-analytics" className={styles.sectionTitle}>
                Persistence Analytics
              </h2>
              <PersistenceAnalytics stats={state.stats} />
            </section>

            {/* Section: Storage & Persistence (Stage 17 detail panels) */}
            <section aria-labelledby="section-storage">
              <h2 id="section-storage" className={styles.sectionTitle}>
                Storage &amp; Persistence
              </h2>
              <div className={styles.detailGrid}>
                <OperationStats stats={state.stats} />
                <PersistencePanel stats={state.stats} />
              </div>
            </section>

            {/* Section: Maintenance */}
            <section aria-labelledby="section-maintenance">
              <h2 id="section-maintenance" className={styles.sectionTitle}>
                Maintenance
              </h2>
              <MaintenancePanel
                onSuccess={fetchData}
                addToast={addToast}
              />
            </section>

            {/* ── BENCHMARK DATA SECTIONS ── */}

            {/* Section: Benchmark Performance */}
            <section aria-labelledby="section-benchmark">
              <h2 id="section-benchmark" className={styles.sectionTitle}>
                Benchmark Performance
              </h2>

              {/* Always show the notice — distinguishes live vs benchmark */}
              {benchState.status === "ready" ? (
                <>
                  <BenchmarkNotice
                    generatedAt={benchState.data.generated_at}
                    forgekvVersion={benchState.data.forgekv_version}
                  />
                  <div className={styles.benchGrid}>
                    <BenchmarkOverview data={benchState.data} />
                    <LatencyPanel data={benchState.data} />
                    <ConcurrencyPanel data={benchState.data} />
                    <HttpPerformancePanel data={benchState.data} />
                    <SnapshotCompactionPanel data={benchState.data} />
                  </div>
                </>
              ) : benchState.status === "unavailable" ? (
                <BenchmarkNotice
                  unavailable
                  reason={benchState.reason}
                />
              ) : (
                <div className={styles.benchLoading}>
                  <Loading label="Loading benchmark results…" size="sm" />
                </div>
              )}
            </section>

            {/* Section: System Information */}
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
