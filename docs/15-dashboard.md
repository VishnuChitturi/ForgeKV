# Stage 15 — Dashboard

## Purpose

Stage 15 turns the placeholder Dashboard page introduced in Stage 14 into a
real operational overview of the ForgeKV engine. The page consumes the
existing `GET /health` and `GET /stats` backend endpoints, formats their
values for human readability, and presents them in a responsive, accessible
layout.

No new backend endpoints were added. No backend code was modified. No
authentication or authorisation was introduced. The Keys and Admin pages
remain placeholders.

---

## Backend Endpoints Used

| Endpoint     | Purpose                                             |
|--------------|-----------------------------------------------------|
| `GET /health`| Determine whether the server is reachable           |
| `GET /stats` | Retrieve all operational metrics from the KV engine |

Both requests are issued in parallel with `Promise.all` on mount and on
every manual refresh.

---

## Statistics Displayed

All fields come directly from the `StatsResponse` type, which mirrors the
C++ backend JSON exactly:

| Backend field            | Displayed as           | Section          |
|--------------------------|------------------------|------------------|
| `key_count`              | Keys                   | Overview + Cards |
| `uptime_seconds`         | Uptime                 | Overview + Cards |
| `wal_size_bytes`         | WAL Size               | Overview + Cards |
| `last_snapshot_time_us`  | Last Snapshot          | Overview + Table |
| `get_hits`               | GET Hits               | Cards + Table    |
| `get_misses`             | GET Misses             | Cards + Table    |
| `set_count`              | SETs                   | Cards + Table    |
| `delete_count`           | Deletes                | Cards + Table    |
| `ttl_set_count`          | TTL Sets               | Table            |
| `expired_count`          | Expired Keys           | Cards + Table    |

---

## Formatting Rules

All formatting helpers live in `frontend/src/utils/format.ts`.

### `formatUptime(seconds: number): string`

Converts an uptime value in (fractional) seconds to a concise duration string.

| Input (s) | Output    |
|-----------|-----------|
| 0         | `0s`      |
| 12        | `12s`     |
| 134       | `2m 14s`  |
| 4320      | `1h 12m`  |
| 187200    | `2d 4h`   |

Only the two most significant units are shown. Seconds are omitted once the
uptime reaches one hour.

### `formatBytes(bytes: number): string`

Displays byte counts with a single decimal place above 1 KB.

| Input     | Output     |
|-----------|------------|
| 0         | `0 B`      |
| 512       | `512 B`    |
| 1229      | `1.2 KB`   |
| 3565158   | `3.4 MB`   |
| 1181116006| `1.1 GB`   |

### `formatNumber(n: number): string`

Locale-aware thousand-separator formatting via `toLocaleString("en-US")`.

| Input    | Output       |
|----------|--------------|
| 12431    | `12,431`     |
| 1234567  | `1,234,567`  |

### `formatSnapshotTime(us: number): string`

`last_snapshot_time_us` is microseconds since Unix epoch.
`0` means the server has never completed a snapshot.

- `0` → `"Never"`
- otherwise → JS `Date` constructed from `us / 1000` (ms), formatted as a
  locale date/time string: e.g. `"Aug 22, 2026, 02:03 PM"`

### `formatLastUpdated(ts: number | null): string`

Frontend-only timestamp: when the browser last successfully received stats.
Has no relation to any backend metric.

- `null` → `"Never"`
- < 5 s ago → `"just now"`
- < 60 s ago → `"14s ago"`
- < 1 h ago → `"3m ago"`
- older → locale time string, e.g. `"02:03 PM"`

---

## Dashboard Layout

```
┌─────────────────────────────────────────────────────┐
│ Page header: title · "Updated: just now" · [Refresh]│
├─────────────────────────────────────────────────────┤
│ Server Overview strip                               │
│  Status · Uptime · Keys · WAL Size · Last Snapshot  │
├─────────────────────────────────────────────────────┤
│ Summary (8 cards)                                   │
│  Keys  Uptime  WAL Size  GET Hits                   │
│  GET Misses  SETs  Deletes  Expired Keys            │
├─────────────────────────────────────────────────────┤
│ Statistics                                          │
│  ┌──────────────────────┐  ┌─────────────────────┐ │
│  │ Operation Statistics │  │ Persistence         │ │
│  │  GET hits            │  │  WAL size           │ │
│  │  GET misses          │  │  Last snapshot      │ │
│  │  SET operations      │  │  Key count          │ │
│  │  DELETE operations   │  │  Uptime             │ │
│  │  TTL sets            │  └─────────────────────┘ │
│  │  Expired keys        │                          │
│  └──────────────────────┘                          │
└─────────────────────────────────────────────────────┘
```

---

## Component Structure

```
frontend/src/
├── utils/
│   └── format.ts               ← formatUptime, formatBytes, formatNumber,
│                                  formatSnapshotTime, formatLastUpdated
├── components/
│   ├── StatCard.tsx             ← single metric card (label + value + icon)
│   ├── StatCard.module.css
│   ├── ServerOverview.tsx       ← horizontal status + metrics strip
│   ├── ServerOverview.module.css
│   ├── StatGroup.tsx            ← titled <dl> of label/value rows
│   └── StatGroup.module.css
└── pages/
    ├── DashboardPage.tsx        ← data fetching, state machine, layout
    └── DashboardPage.module.css
```

Existing Stage 14 components used unchanged:

- `Loading` — shown during the initial fetch
- `ErrorMessage` — shown when `GET /stats` fails, includes a retry callback
- `ServerStatusBadge` — in the top bar (unchanged)

---

## Refresh Behaviour

1. On component mount, `fetchData()` fires once.
2. `GET /health` and `GET /stats` run in parallel via `Promise.all`.
3. The Refresh button in the page header triggers `fetchData()` again.
4. On re-fetch (i.e. after the initial load), the existing data remains
   visible at reduced opacity (`contentRefreshing` CSS class) while the
   request is in flight. The button label changes to "Refreshing…" and is
   disabled until the response arrives.
5. On success, `lastUpdated` is set to `Date.now()`. The "Updated: just now"
   label reflects this frontend-side timestamp.
6. No automatic polling. No websockets.

---

## Loading State

On the first load the page body is replaced by a centred `<Loading size="lg">`
spinner. No placeholder metric values are shown. Once the first successful
response is received, the data renders and subsequent refreshes use the
subtle opacity-fade approach described above.

---

## Error State

If `GET /stats` returns a non-2xx response or the network request fails:

- The page body shows `<ErrorMessage>` with the server error message (or a
  fallback string).
- A "Try again" button is rendered by `ErrorMessage` and is wired to
  `fetchData()`.
- If `GET /health` fails but `GET /stats` succeeds, the server overview
  shows "Offline" status but the statistics are still displayed.
- The rest of the application (navigation, other pages) continues to work.

---

## Accessibility

- Page sections use `<section>` with `aria-labelledby` pointing to an `<h2>`.
- The Server Overview section uses `aria-label`.
- `StatCard` renders as `<article>` with `aria-label="${label}: ${value}"`.
- `StatGroup` renders as `<section>` with `aria-label="${title}"` and uses
  a `<dl>/<dt>/<dd>` definition list for semantic key/value pairs.
- The Refresh button has `aria-label="Refresh dashboard"`.
- The "Updated: …" label uses `aria-live="polite"` so screen readers announce
  updates.
- All interactive elements are keyboard-accessible and have visible
  `:focus-visible` outlines.
- Status is conveyed by both colour and text label — no colour-only indicators.

---

## Responsive Behaviour

The layout uses CSS Grid `auto-fill` with `minmax()` so all grids wrap
naturally without media query breakpoints for most screen widths.

Explicit breakpoints at 639 px (matching the existing AppLayout sidebar
collapse):

- Header row stacks vertically (title above actions).
- Summary card minimum column width reduces from 10 rem to 8 rem.
- `StatGroup` grid collapses to a single column.
- `ServerOverview` items stack vertically and dividers are hidden.

No horizontal scrolling is introduced at any viewport width.

---

## API Architecture

Data fetching uses `getHealth()` and `getStats()` from `frontend/src/services/api.ts`.
No `fetch()` calls appear directly in `DashboardPage.tsx`. The `ApiResult<T>`
discriminated union is handled at the call site:

```typescript
const [healthResult, statsResult] = await Promise.all([
  getHealth(),   // ApiResult<HealthResponse>
  getStats(),    // ApiResult<StatsResponse>
]);
```

If `statsResult.ok === false`, the dashboard transitions to the error state.
If only `healthResult.ok === false`, the server status shows "Offline" but
statistics are displayed as received.

---

## Known Limitations

1. **No automatic refresh.** The dashboard shows a snapshot in time. Users
   must click Refresh to see updated values.

2. **Large numbers.** `key_count`, `get_hits`, etc. are `uint64` on the
   backend. JavaScript numbers lose precision above
   `Number.MAX_SAFE_INTEGER` (2^53 − 1 ≈ 9 quadrillion). For realistic
   ForgeKV workloads this is not a concern, but it is documented.

3. **`last_snapshot_time_us` timezone.** The snapshot timestamp is
   displayed in the browser's local timezone via `Date.toLocaleString`.
   It is not UTC. This is intentional (the user's machine time is the most
   useful reference) but should be noted.

4. **Keys and Admin pages** remain placeholders. Stage 15 scope is
   dashboard only.

5. **No offline fallback cache.** If the server goes offline between page
   loads, previously seen statistics are lost. No stale-data display is
   implemented.

---

## No Backend Changes

The C++ backend was not modified. `GET /stats` already returns all required
fields. Frontend types in `src/types/api.ts` were confirmed to match the
backend response exactly — no changes were needed.

Backend version remains: **v0.13.0**
Frontend version remains: **0.1.x**
