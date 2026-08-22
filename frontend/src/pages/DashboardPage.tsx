// =============================================================================
// DashboardPage — Stage 15 (polished Stage 19)
//
// Fetches GET /health and GET /stats on mount and on manual refresh.
// No polling. No websockets. No global state library.
//
// Stage 19 changes:
//   - headerRow, headerActions, lastUpdated, refreshBtn, refreshIcon,
//     refreshIconSpin, loadingCenter, errorWrapper, content,
//     contentRefreshing, sectionTitle now come from Page.module.css.
// =============================================================================

import { useCallback, useEffect, useRef, useState } from "react";
import { ErrorMessage } from "../components/ErrorMessage";
import { Loading } from "../components/Loading";
import { ServerOverview } from "../components/ServerOverview";
import { StatCard } from "../components/StatCard";
import { StatGroup } from "../components/StatGroup";
import type { ServerStatus } from "../hooks/useServerStatus";
import { getHealth, getStats } from "../services/api";
import type { StatsResponse } from "../types/api";
import {
  formatBytes,
  formatLastUpdated,
  formatNumber,
  formatSnapshotTime,
  formatUptime,
} from "../utils/format";
import styles from "./DashboardPage.module.css";
import pageStyles from "./Page.module.css";

// ---------------------------------------------------------------------------
// Dashboard state
// ---------------------------------------------------------------------------
type DashboardState =
  | { phase: "loading" }
  | { phase: "error"; message: string }
  | { phase: "ready"; stats: StatsResponse; serverStatus: ServerStatus };

export function DashboardPage() {
  const [state, setState] = useState<DashboardState>({ phase: "loading" });
  const [refreshing, setRefreshing] = useState(false);
  const [lastUpdated, setLastUpdated] = useState<number | null>(null);

  // Tracks whether the initial load has completed at least once.
  const hasLoadedRef = useRef(false);

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
      setState({
        phase: "error",
        message: statsResult.error || "Unable to load ForgeKV statistics.",
      });
      return;
    }

    const serverStatus: ServerStatus = healthResult.ok ? "connected" : "offline";
    hasLoadedRef.current = true;
    setState({ phase: "ready", stats: statsResult.data, serverStatus });
    setLastUpdated(Date.now());
  }, []);

  useEffect(() => {
    fetchData();
  }, [fetchData]);

  // -------------------------------------------------------------------------
  // Render
  // -------------------------------------------------------------------------
  return (
    <div className={pageStyles.page}>
      {/* -- Page header -- */}
      <header className={pageStyles.pageHeader}>
        <div className={pageStyles.headerRow}>
          <div>
            <h1 className={pageStyles.pageTitle}>Dashboard</h1>
            <p className={pageStyles.pageDescription}>
              Real-time operational overview of the ForgeKV engine.
            </p>
          </div>
          <div className={pageStyles.headerActions}>
            {lastUpdated !== null && (
              <span className={pageStyles.lastUpdated} aria-live="polite">
                Updated: {formatLastUpdated(lastUpdated)}
              </span>
            )}
            <button
              className={pageStyles.refreshBtn}
              type="button"
              onClick={fetchData}
              disabled={refreshing || state.phase === "loading"}
              aria-label="Refresh dashboard"
            >
              <span
                className={refreshing ? pageStyles.refreshIconSpin : pageStyles.refreshIcon}
                aria-hidden="true"
              >
                ↻
              </span>
              {refreshing ? "Refreshing…" : "Refresh"}
            </button>
          </div>
        </div>
      </header>

      {/* -- Page body -- */}
      <div className={pageStyles.pageBody}>
        {/* Loading state — only shown on first load */}
        {state.phase === "loading" && (
          <div className={pageStyles.loadingCenter}>
            <Loading label="Loading dashboard…" size="lg" />
          </div>
        )}

        {/* Error state */}
        {state.phase === "error" && (
          <div className={pageStyles.errorWrapper}>
            <ErrorMessage
              title="Unable to load ForgeKV statistics."
              message={state.message}
              onRetry={fetchData}
            />
          </div>
        )}

        {/* Ready state */}
        {state.phase === "ready" && (
          <div className={`${pageStyles.content} ${refreshing ? pageStyles.contentRefreshing : ""}`}>
            {/* 1. Server Overview strip */}
            <section aria-labelledby="overview-heading">
              <h2 id="overview-heading" className={pageStyles.sectionTitle}>
                Server Overview
              </h2>
              <ServerOverview
                serverStatus={state.serverStatus}
                stats={state.stats}
              />
            </section>

            {/* 2. Summary cards */}
            <section aria-labelledby="summary-heading">
              <h2 id="summary-heading" className={pageStyles.sectionTitle}>
                Summary
              </h2>
              <div className={styles.cardGrid}>
                <StatCard
                  icon="🗝"
                  label="Keys"
                  value={formatNumber(state.stats.key_count)}
                  accent="neutral"
                />
                <StatCard
                  icon="⏱"
                  label="Uptime"
                  value={formatUptime(state.stats.uptime_seconds)}
                  accent="neutral"
                />
                <StatCard
                  icon="💾"
                  label="WAL Size"
                  value={formatBytes(state.stats.wal_size_bytes)}
                  accent="neutral"
                />
                <StatCard
                  icon="✅"
                  label="GET Hits"
                  value={formatNumber(state.stats.get_hits)}
                  accent="ok"
                />
                <StatCard
                  icon="✗"
                  label="GET Misses"
                  value={formatNumber(state.stats.get_misses)}
                  accent={state.stats.get_misses > 0 ? "warning" : "neutral"}
                />
                <StatCard
                  icon="✎"
                  label="SETs"
                  value={formatNumber(state.stats.set_count)}
                  accent="neutral"
                />
                <StatCard
                  icon="🗑"
                  label="Deletes"
                  value={formatNumber(state.stats.delete_count)}
                  accent="neutral"
                />
                <StatCard
                  icon="⌛"
                  label="Expired Keys"
                  value={formatNumber(state.stats.expired_count)}
                  accent={state.stats.expired_count > 0 ? "warning" : "neutral"}
                />
              </div>
            </section>

            {/* 3. Detail statistics */}
            <section aria-labelledby="stats-heading">
              <h2 id="stats-heading" className={pageStyles.sectionTitle}>
                Statistics
              </h2>
              <div className={styles.statGroupGrid}>
                <StatGroup
                  title="Operation Statistics"
                  rows={[
                    { label: "GET hits",          value: formatNumber(state.stats.get_hits) },
                    { label: "GET misses",         value: formatNumber(state.stats.get_misses) },
                    { label: "SET operations",     value: formatNumber(state.stats.set_count) },
                    { label: "DELETE operations",  value: formatNumber(state.stats.delete_count) },
                    { label: "TTL sets",           value: formatNumber(state.stats.ttl_set_count) },
                    { label: "Expired keys",       value: formatNumber(state.stats.expired_count) },
                  ]}
                />
                <StatGroup
                  title="Persistence"
                  rows={[
                    { label: "WAL size",       value: formatBytes(state.stats.wal_size_bytes) },
                    { label: "Last snapshot",  value: formatSnapshotTime(state.stats.last_snapshot_time_us) },
                    { label: "Key count",      value: formatNumber(state.stats.key_count) },
                    { label: "Uptime",         value: formatUptime(state.stats.uptime_seconds) },
                  ]}
                />
              </div>
            </section>
          </div>
        )}
      </div>
    </div>
  );
}
