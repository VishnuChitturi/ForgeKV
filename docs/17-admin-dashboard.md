# Stage 17 — Admin Dashboard

## Purpose

The Admin Dashboard (`/admin`) is an **operational control and monitoring page** for the ForgeKV engine. It is distinct from the Stage 15 Dashboard (`/`) which provides a high-level user-facing overview.

The Admin page answers the questions an operator would ask:

- Is the ForgeKV server healthy?
- What is its current state?
- How much data is it handling?
- How is persistence doing?
- What operational actions are available?
- What version information and system details are available?

It is **not** an analytics dashboard. Charts, time-series visualization, and throughput graphs belong to Stage 18.

---

## Sections

### 1. Server Health

**Component:** `AdminHealth`

Displays the operational health state derived from `GET /health` and `GET /stats`:

| Field         | Source                          |
|---------------|---------------------------------|
| Server status | `GET /health` response success  |
| Uptime        | `stats.uptime_seconds`          |
| Live Keys     | `stats.key_count`               |
| WAL Size      | `stats.wal_size_bytes`          |
| Last Snapshot | `stats.last_snapshot_time_us`   |

The status indicator uses a coloured dot: green (Online), red (Offline), amber (Checking). Status text is colour-independent for accessibility.

### 2. Storage & Persistence

Two sub-panels side by side (wrap on small screens):

**OperationStats** — All six operation counters from `GET /stats`:

| Counter          | Backend field        |
|------------------|----------------------|
| GET hits         | `get_hits`           |
| GET misses       | `get_misses`         |
| Hit rate         | Computed: hits/(hits+misses) |
| SET operations   | `set_count`          |
| DELETE operations| `delete_count`       |
| TTL sets         | `ttl_set_count`      |
| Expired keys     | `expired_count`      |

No counter meanings are changed. No counters are reset or reinterpreted.

**PersistencePanel** — Persistence-relevant fields:

| Field            | Backend field               |
|------------------|-----------------------------|
| WAL size         | `stats.wal_size_bytes`      |
| Last snapshot    | `stats.last_snapshot_time_us` |
| Live key count   | `stats.key_count`           |
| Expired key count| `stats.expired_count`       |

### 3. Maintenance

**Component:** `MaintenancePanel`

Provides two operational controls:

#### Create Snapshot (`POST /snapshot`)

- Triggers `KeyValueStore::snapshot()` via the new HTTP endpoint.
- Requires a confirmation dialog before executing.
- Shows a spinner on the button while in-flight.
- On success: shows a success toast and refreshes `/stats`.
- On failure: shows an error toast; page remains functional.
- The button is disabled while any maintenance operation is in-flight, preventing double-triggers.

#### Compact WAL (`POST /compact`)

- Triggers `KeyValueStore::compact()` via the new HTTP endpoint.
- Requires explicit confirmation with a warning dialog describing that it rewrites the WAL and may briefly block writes.
- Shows `Compacting…` with a spinner while in-flight.
- On success: shows a success toast and refreshes `/stats`.
- On failure: shows an error toast.
- Button is disabled while in-flight.

### 4. System Information

**Component:** `SystemInfo`

Displays information that is actually available and reliable:

| Field              | Value / Source                              |
|--------------------|---------------------------------------------|
| Backend version    | `v0.13.0` (project constant, CMakeLists.txt)|
| Frontend version   | `v0.1.0` (package.json)                     |
| HTTP server        | Derived from live `/health` result          |
| Persistence model  | Binary WAL + Snapshots (stable project fact)|
| Concurrency model  | `std::shared_mutex` (stable project fact)   |
| HTTP library       | cpp-httplib v0.18.5 (vendored)              |

CPU, memory, disk, and network metrics are **not shown** because the backend does not provide them. A note on the panel explicitly states this.

---

## Backend Endpoints Used

| Endpoint        | Purpose                                      |
|-----------------|----------------------------------------------|
| `GET /health`   | Determine server online/offline status       |
| `GET /stats`    | All operational metrics                      |
| `POST /snapshot`| Trigger a full-state checkpoint (Stage 17)   |
| `POST /compact` | Trigger WAL compaction (Stage 17)            |

---

## New API Endpoints (Stage 17)

### `POST /snapshot`

```
POST /snapshot
→ 200 {"status":"ok"}
→ 500 {"error":"snapshot failed"}    // if snapshot() returns false
→ 500 {"error":"<message>"}          // on exception
```

- Calls `KeyValueStore::snapshot()` directly — no new snapshot logic.
- Uses the existing locking semantics inside `snapshot()` (exclusive write lock).
- No HTTP-level mutex is added.
- After success, clients should call `GET /stats` to see the updated `last_snapshot_time_us`.

### `POST /compact`

```
POST /compact
→ 200 {"status":"ok"}
→ 500 {"error":"<message>"}          // on exception from compact()
```

- Calls `KeyValueStore::compact()` directly — no new compaction logic.
- Uses the existing locking semantics inside `compact()`.
- No HTTP-level mutex is added.
- `compact()` does not return a bool; it throws on failure. The HTTP layer catches any exception and returns 500.

---

## Snapshot Behaviour

`KeyValueStore::snapshot()`:

- Acquires the exclusive write lock.
- Writes all live (non-expired) keys to a snapshot file (`<wal_path>.snapshot`).
- Expiring-but-live keys are stored with their expiry metadata.
- Updates `last_snapshot_time_us` atomically on success.
- Returns `true` on success, `false` on failure.
- Is safe to call repeatedly — each call replaces the previous snapshot file.

The HTTP endpoint surfaces the return value: `false` → HTTP 500.

After calling `POST /snapshot`, the `last_snapshot_time_us` field in `GET /stats` will reflect the new timestamp.

---

## Compaction Behaviour

`KeyValueStore::compact()`:

- Acquires the exclusive write lock.
- Rewrites the WAL to contain only the current live state.
- Expired keys are excluded from the compacted WAL.
- Live expiring keys are preserved with their `SET_WITH_EXPIRY` records.
- Deletes the existing snapshot file before rewriting (as designed in Stage 9).
- May temporarily block concurrent write operations while the WAL is being rewritten.
- Throws `std::runtime_error` on I/O failure.

The HTTP endpoint wraps the call in try/catch: any exception → HTTP 500.

---

## Concurrency Considerations

- Both `POST /snapshot` and `POST /compact` delegate **all** synchronization to `KeyValueStore`.
- No additional HTTP-level mutex is introduced.
- `KeyValueStore` uses `std::shared_mutex` — snapshot and compact acquire the exclusive write lock; concurrent `GET` requests hold shared locks and are blocked only for the duration of the operation.
- cpp-httplib's default ThreadPool handles concurrent HTTP requests safely.
- The UI disables both maintenance buttons while any operation is in-flight, preventing duplicate HTTP requests from the same browser tab. The backend handles concurrent requests correctly regardless.

---

## Refresh Behaviour

- Manual refresh only. No automatic polling. No WebSockets.
- The Refresh button fires `GET /health` and `GET /stats` in parallel (`Promise.all`).
- "Last updated" timestamp is a frontend `Date.now()` value — it reflects when the data was last fetched, not the backend's clock.
- Formatting uses `formatLastUpdated()` from `utils/format.ts` (Stage 15).
- While refreshing: the content fades to 65% opacity and pointer events are disabled to prevent interaction with stale state.
- If the refresh fails after the page has already loaded, an error toast is shown and the last successful data remains visible.

---

## Error Handling

| Scenario                   | Behaviour                                        |
|----------------------------|--------------------------------------------------|
| Backend offline on load    | Full-page ErrorMessage with Retry button         |
| `/stats` fails on load     | Full-page ErrorMessage with Retry button         |
| `/health` fails on load    | Stats still shown; server status shows Offline   |
| Refresh fails              | Error toast; last successful data stays visible  |
| `POST /snapshot` fails     | Error toast; page remains functional             |
| `POST /compact` fails      | Error toast; page remains functional             |

---

## Available System Information

Only data the backend actually provides is shown. The following are explicitly excluded because the backend does not expose them:

- CPU usage
- Memory usage
- Disk usage / free space
- Network I/O
- Request rate / latency
- Throughput

A note in the SystemInfo panel informs operators of this limitation.

---

## Responsive Design

- All panels use CSS Grid with `auto-fill` and `minmax` for natural wrapping.
- The detail grid (OperationStats + PersistencePanel) collapses to a single column below 640px.
- Maintenance action rows stack vertically on mobile, with full-width buttons.
- The page header wraps gracefully; refresh button remains accessible.
- Confirmation dialogs use `position: fixed` with `calc(100vw - 2rem)` width cap, ensuring they are usable on small screens.

---

## Accessibility

- Section headings use `aria-labelledby` on `<section>` elements.
- Status indicators use both colour and text labels — colour is not the sole differentiator.
- Confirmation dialogs use `role="dialog"` with `aria-modal="true"` and `aria-labelledby`.
- Focus is moved to the Cancel button when a dialog opens.
- The Escape key closes the dialog without action.
- All buttons have explicit `aria-label` values.
- The `Toast` container uses `aria-live="polite"` for screen reader announcements.
- Spinner elements use `aria-hidden="true"`.
- The refresh button has a descriptive `aria-label`.
- `:focus-visible` ring is inherited from global.css.

---

## Component Structure

```
AdminPage
  ├── AdminHealth           — server status + uptime + key metrics
  ├── OperationStats        — GET/SET/DELETE/TTL/expiry counters
  ├── PersistencePanel      — WAL size, snapshot time, key/expiry counts
  ├── MaintenancePanel      — Create Snapshot + Compact WAL controls
  └── SystemInfo            — version info, HTTP server status, tech stack
```

Reused from earlier stages:
- `StatGroup` (Stage 15) — used by OperationStats and PersistencePanel
- `Loading`, `ErrorMessage` (Stage 14) — first-load states
- `Toast`, `useToast` (Stage 16) — operation feedback
- `formatUptime`, `formatBytes`, `formatNumber`, `formatSnapshotTime`, `formatLastUpdated` (Stage 15)

---

## Known Limitations

1. **Backend version is a hard-coded constant.** It is accurate (matches `CMakeLists.txt` `PROJECT_VERSION 0.13.0`) but is not fetched at runtime. Adding a `GET /version` endpoint was not justified for this stage.

2. **No automatic polling.** The admin page requires a manual refresh to see updated stats. This is intentional — Stage 17 is operational, not real-time monitoring.

3. **No resource metrics.** CPU, memory, disk, and network information are not available from the ForgeKV backend. They are not shown rather than fabricated.

4. **Compact does not return a WAL size delta.** After compaction the operator must manually refresh to see the updated WAL size. This is a property of the existing backend API, not a Stage 17 limitation.

5. **Stage 18 analytics not included.** Charts, graphs, time-series data, and throughput visualization are explicitly deferred to Stage 18.

---

## Test Coverage

`tests/test_http_admin.cpp` — 14 integration tests:

| # | Test                                         |
|---|----------------------------------------------|
| 1 | `POST /snapshot` returns 200 `{"status":"ok"}` |
| 2 | Snapshot updates `last_snapshot_time_us` in `/stats` |
| 3 | Data survives snapshot (keys still readable) |
| 4 | Repeated snapshots all succeed               |
| 5 | `POST /compact` returns 200 `{"status":"ok"}` |
| 6 | Compact preserves all live keys              |
| 7 | Compact on empty store succeeds              |
| 8 | Keys set before compact readable after       |
| 9 | Deleted keys not present after compact       |
| 10| Snapshot then compact — both succeed, data intact |
| 11| Concurrent reads during snapshot — no crash  |
| 12| Concurrent writes during compact — no crash, state consistent |
| 13| Other endpoints still work after snapshot    |
| 14| Other endpoints still work after compact     |

CTest target: `forgekv_http_admin_tests`

All 14/14 tests pass. All 14 pre-existing CTest targets continue passing.
