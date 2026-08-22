# Stage 19 — UI Polish & Integration

## Overview

Stage 19 is a horizontal polish and integration stage for the ForgeKV frontend.
No new features were added. The goal was to audit every part of the frontend,
fix inconsistencies, eliminate duplication, and ensure the application feels
like a single finished product across all three routes.

**Backend: zero changes.** All 14/14 CTest targets pass unchanged.

---

## Route Structure

| Path     | Page               | Status                     |
|----------|--------------------|----------------------------|
| `/`      | Dashboard          | ✅ Live (Stage 15)          |
| `/keys`  | Key Management     | ✅ Live (Stage 16)          |
| `/admin` | Admin / Operator   | ✅ Live (Stage 17–18)       |
| `/*`     | Catch-all → `/`    | ✅ Redirect                 |

Browser refresh on any route works correctly. Unknown paths redirect to `/`.

---

## Frontend Architecture

```
frontend/src/
├── App.tsx                      React Router root
├── main.tsx                     StrictMode + BrowserRouter mount
├── styles/
│   └── global.css               Design tokens (CSS custom properties) + reset
├── layouts/
│   ├── AppLayout.tsx            Shell: topbar + sidebar + main outlet
│   └── AppLayout.module.css     Shell styles + skip-link + srOnly
├── pages/
│   ├── Page.module.css          ★ Shared page-level styles (Stage 19)
│   ├── DashboardPage.tsx        GET /health + GET /stats dashboard
│   ├── DashboardPage.module.css Card grid, stat group grid
│   ├── KeysPage.tsx             Full CRUD key management
│   ├── KeysPage.module.css      Table, search, form, modal styles
│   ├── AdminPage.tsx            Operator console + analytics + benchmarks
│   └── AdminPage.module.css     Detail grid, benchmark grid
├── components/
│   ├── Loading.tsx / .module.css        Spinner
│   ├── ErrorMessage.tsx / .module.css   Error with retry
│   ├── Empty.tsx / .module.css          Empty state
│   ├── Modal.tsx / .module.css          ★ Focus-trapped dialog (Stage 19)
│   ├── Toast.tsx / .module.css          Toast notifications
│   ├── ServerStatus.tsx / .module.css   Status badge (topbar)
│   ├── StatCard.tsx / .module.css       Summary metric card
│   ├── StatGroup.tsx / .module.css      ★ h3 heading fix (Stage 19)
│   ├── ServerOverview.tsx / .module.css Horizontal status strip
│   ├── AdminHealth.tsx / .module.css    Admin health section
│   ├── MaintenancePanel.tsx / .module.css Snapshot + compact controls
│   ├── OperationStats.tsx               Ops counters (uses StatGroup)
│   ├── PersistencePanel.tsx             Persistence info (uses StatGroup)
│   ├── SystemInfo.tsx / .module.css     Version + model info
│   ├── AnalyticsOverview.tsx            HitRateCard + OperationBreakdown
│   ├── HitRateCard.tsx / .module.css    GET hit rate donut
│   ├── OperationBreakdown.tsx / .module.css CSS bar chart
│   ├── PersistenceAnalytics.tsx / .module.css Persistence metrics grid
│   ├── BenchmarkNotice.tsx / .module.css Benchmark data disclaimer
│   ├── BenchmarkOverview.tsx / .module.css KV throughput table
│   ├── LatencyPanel.tsx / .module.css   Latency percentile table
│   ├── ConcurrencyPanel.tsx / .module.css Concurrent scaling table
│   ├── HttpPerformancePanel.tsx / .module.css HTTP benchmark table
│   └── SnapshotCompactionPanel.tsx / .module.css Snapshot/compaction times
├── hooks/
│   └── useServerStatus.ts       Single health check on mount
├── services/
│   └── api.ts                   Centralized, typed API client
├── types/
│   ├── api.ts                   Backend JSON types
│   └── benchmark.ts             Benchmark artifact types
└── utils/
    ├── format.ts                Number/time/byte formatters
    └── benchmark.ts             Benchmark loader + query helpers
```

---

## Changes Made in Stage 19

### Application Shell (AppLayout)

- **Footer text updated** from "Stage 16" to "Stage 19".
- **Skip-to-main-content link** added for keyboard users. Hidden until
  focused; jumps to `#main-content`. Styled in `AppLayout.module.css`.
- **aria-current fix**: NavLink previously set `aria-current={undefined}`
  explicitly, which is technically redundant. The implementation now lets
  NavLink handle active state naturally and adds a screen-reader-only
  "(current page)" span for assistive technology.
- **`.srOnly`** utility class added to AppLayout.module.css for any
  visually-hidden text needed across the layout.

### Shared Page Styles (Page.module.css)

Extracted 15 CSS class definitions that were duplicated between
`DashboardPage.module.css` and `AdminPage.module.css`:

- `.headerRow`, `.headerActions`, `.lastUpdated`
- `.refreshBtn`, `.refreshIcon`, `.refreshIconSpin`, `@keyframes spin`
- `.loadingCenter`, `.errorWrapper`
- `.content`, `.contentRefreshing`
- `.sectionTitle`, `.sectionDesc`
- `.btn`, `.btnSecondary`, `.btnPrimary`, `.btnDanger`

Both `DashboardPage.tsx` and `AdminPage.tsx` now import these from
`Page.module.css`. `KeysPage.tsx` uses `pageStyles.btnSecondary`, `.btnPrimary`,
`.btnDanger` instead of its own duplicated definitions.

### Modal (Modal.tsx)

- **Focus trap**: Tab and Shift+Tab now cycle focus only within the open
  dialog. Previously focus could escape the modal via Tab.
- **aria-modal / role="dialog"** moved from the overlay `<div>` to the
  panel `<div>`. The overlay now has `role="presentation"` so assistive
  technology correctly identifies the panel as the dialog.
- **Unique title IDs**: `useId()` generates a unique ID per Modal instance,
  replacing the hard-coded `"modal-title"` id. This prevents duplicate-id
  violations if multiple Modal components are mounted simultaneously.
- **First-focusable-element focus**: On open, focus moves to the first
  naturally focusable element inside the dialog rather than the panel
  container itself, so keyboard users land on the first input or button.

### StatGroup (StatGroup.tsx)

- Heading changed from **`h2` to `h3`**. StatGroup is always rendered inside
  a section that already has a `h2` heading. Using `h2` here created
  same-level headings that broke the document outline and heading hierarchy.

### AdminPage (AdminPage.tsx)

- **Page title corrected** from "Analytics & Performance" to "Admin".
  The route is `/admin` and the page is the operator console, not an
  analytics-only page. Description updated to reflect all sections.
- **Benchmark initial state** changed from `{ status: "idle" }` to
  `{ status: "loading" }`. The previous code caused a brief flash of empty
  content between initial render and the first useEffect tick.
- **Duplicate section headings eliminated**: AdminHealth, MaintenancePanel,
  and SystemInfo each have their own internal `h2` heading. AdminPage
  previously also added a `h2` above each of these components, creating
  double headings. The page-level `h2`s were removed; the `<section>`
  elements now use `aria-labelledby` pointing to the component-internal
  heading IDs.
- **Shared Page styles** used for header row, refresh button, loading
  center, error wrapper, content/contentRefreshing, sectionTitle, and
  sectionDesc.

### DashboardPage (DashboardPage.tsx)

- Migrated all duplicated CSS classes to `pageStyles` from `Page.module.css`.
- `DashboardPage.module.css` now contains only `.cardGrid` and
  `.statGroupGrid` — the two grids that are unique to this page.

### KeysPage (KeysPage.tsx)

**Bug fix: searchInput not syncing from URL on back/forward navigation**
The `searchInput` state was only initialised from `urlPrefix` on mount.
When the user pressed browser Back (which changes the URL without remounting),
`prefix` and `page` updated correctly, but `searchInput` stayed stale,
showing old text in the search field. The URL-sync `useEffect` now also calls
`setSearchInput(p)`.

**Bug fix: pagination after delete may leave user on an empty page**
When a user deleted the last key on the last page, `total` decreased but
`page` stayed the same. `totalPages` recalculated correctly but the fetch
effect only triggers on `prefix`/`page` changes — not on `total` changes.
The delete handler now computes `newTotal` and `newTotalPages` locally and
navigates to the clamped page via `setSearchParams` if the current page
exceeds the new page count.

**Shared button styles**: `btnSecondary`, `btnPrimary`, `btnDanger` now
imported from `pageStyles` (Page.module.css). KeysPage.module.css retains
only page-specific styles: search field, table, form fields, modals, confirm
dialog, pagination, and the `btnIcon`/`btnIconSpin` animation classes.

---

## API Integration

No changes to `api.ts` or `useServerStatus.ts`. The API client was already:

- Typed with `ApiResult<T>` discriminated union
- Centralized — no scattered `fetch()` calls in components
- Error-aware — all failures surfaced as `{ ok: false, error }`, never thrown
- Consistent — one `request<T>()` helper for all endpoints

The `VITE_API_BASE_URL` / Vite dev proxy setup remains unchanged. All API
calls go through `/api` which the proxy rewrites to the backend root.

---

## Responsive Strategy

The application uses CSS custom properties for layout dimensions
(`--topbar-height`, `--sidebar-width`, `--sidebar-width-collapsed`).

At ≤639px:
- Sidebar collapses to icon-only view (brand name and nav labels hidden)
- Page headers stack vertically (`.headerRow` → `flex-direction: column`)
- Card grids reduce minimum width (`10rem → 8rem`)
- Stat group grids go single-column
- Key table hides the Value and TTL columns (most important info remains)
- Toolbar stacks vertically
- Page body padding reduced to 1rem

The admin page benchmark tables allow internal horizontal scrolling on
narrow viewports via `overflow-x: auto` on `.tableWrapper` in each benchmark
component's CSS module.

---

## Accessibility Improvements

| Area | Change |
|------|--------|
| Skip link | Added skip-to-main-content for keyboard users |
| Modal focus trap | Tab/Shift+Tab now cycle within the open dialog |
| Modal ARIA | `aria-modal`/`role="dialog"` moved to the panel element |
| Modal IDs | `useId()` generates unique `aria-labelledby` IDs per instance |
| Modal first focus | Focus lands on first actionable element in the dialog |
| Heading hierarchy | `StatGroup` h2→h3; AdminPage removed duplicate section h2s |
| Screen reader text | `.srOnly` utility available for visually-hidden text |
| Page title | AdminPage title corrected to "Admin" (was misleading) |
| Nav current | Screen-reader-only "(current page)" text on active nav item |
| Benchmark status | `role="note"` on BenchmarkNotice; empty state uses `role="status"` |
| Operation bars | `role="list"` with `aria-label` per bar row in OperationBreakdown |
| Charts | SVG donut `aria-label` describes the data; numeric values in text |

---

## Error / Loading Strategy

Each page follows a consistent state machine:

```
loading → ready
        → error → retry → loading → ready | error
```

On subsequent refreshes after first load:
- Full loading overlay is NOT shown (no layout shift)
- A subtle `contentRefreshing` opacity fade indicates background activity
- Errors on refresh show as toasts (AdminPage) or inline error state (Dashboard)

Benchmark loading:
- State initializes as `"loading"` (not `"idle"`) to prevent flash
- `BenchmarkNotice` shows graceful unavailable state with instructions if
  `benchmark-results.json` is missing, malformed, or uses an unsupported schema
- Benchmark load never blocks or delays live data display

---

## End-to-End Verification

Manual test performed with `./build/forgekv_server` and `npm run dev`:

| Step | Result |
|------|--------|
| Open Dashboard | ✅ Loads, shows health, stats, last-updated |
| Verify stats | ✅ Correct numbers from /stats |
| Navigate to Keys | ✅ Table loads, pagination info correct |
| Create key | ✅ PUT succeeds, toast shown, table refreshes |
| View key | ✅ Full value in modal, TTL shown |
| Edit key value + TTL | ✅ PUT succeeds, updated row visible |
| Verify TTL display | ✅ "45s", "Permanent", "Expiring…" correctly formatted |
| Search by prefix | ✅ ?prefix= updates URL, table filters |
| Pagination | ✅ Pages navigate, count shown |
| Delete key | ✅ Confirmation modal, success toast, table refreshes |
| Verify deletion | ✅ Key absent from list |
| Navigate to Admin | ✅ "Admin" title, all sections visible |
| Live stats | ✅ Same values as dashboard with refresh |
| Create Snapshot | ✅ Confirmation dialog, success toast, stats refresh |
| Compact WAL | ✅ Warning dialog, success toast, stats refresh |
| Benchmark section | ✅ Notice shown, clearly labeled as benchmark data |
| Benchmark vs live | ✅ Clear visual and textual separation |
| Browser refresh on / | ✅ Works |
| Browser refresh on /keys | ✅ Works, URL state restored |
| Browser refresh on /admin | ✅ Works |
| Stop backend | ✅ Status badge → Offline, error state on pages |
| Restart backend | ✅ Refresh shows connected, data loads |

---

## Known Limitations

1. **No polling**: All data is fetched on mount and manual refresh only.
   Stale data is possible if the server state changes while the page is open
   and the user has not refreshed. This is intentional per Stage 19 scope.

2. **useServerStatus independence**: The AppLayout top bar runs its own
   `useServerStatus()` hook, which is independent from the per-page
   health checks. This means up to two `/health` calls on initial page load.
   Sharing context would require a state management layer not introduced in
   Stage 19 scope.

3. **No optimistic updates**: Mutations wait for the backend round-trip before
   updating the UI. This is correct and safe, not a limitation.

4. **Key search is prefix-only**: The backend only supports prefix filtering.
   Substring or full-text search would require a backend change.

5. **Benchmark data is static**: Benchmark results are a pre-generated
   artifact copied to `frontend/public/`. They do not update automatically
   when the backend changes. Run `./build/forgekv_benchmark --json-output
   frontend/public/benchmark-results.json` to refresh.

---

## Verification Results

```
TypeScript: 0 errors
Build:      93 modules, 560ms, no warnings
Backend:    14/14 CTest targets pass
```
