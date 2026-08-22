# ForgeKV — Stage 16: Key Management

> **Status:** Complete  
> **Backend version:** 0.13.0 (unchanged — new endpoint extends existing surface)

---

## Overview

Stage 16 delivers the complete Key Management experience for ForgeKV.

Users can now:

- Browse all live keys stored in the engine
- Filter by key prefix
- Paginate through large key sets
- View full key details (value + TTL)
- Create keys with optional TTL
- Edit existing keys (value and/or TTL)
- Delete keys with confirmation
- See real-time success/error feedback via toast notifications

No fake data. Every operation talks directly to the ForgeKV backend.

---

## Part A — GET /keys Endpoint

### Route

```
GET /keys
```

### Query Parameters

| Parameter | Type    | Default | Maximum | Description                                |
|-----------|---------|---------|---------|--------------------------------------------|
| `prefix`  | string  | `""`    | —       | Return only keys starting with this value  |
| `limit`   | integer | `50`    | `100`   | Maximum keys to return per page            |
| `offset`  | integer | `0`     | —       | Skip this many results (for pagination)    |

### Response Shape (200 OK)

```json
{
  "keys": [
    {
      "key":         "user:1001",
      "value":       "Vishnu",
      "ttl_seconds": -1.0
    },
    {
      "key":         "session:abc",
      "value":       "active",
      "ttl_seconds": 3542.812
    }
  ],
  "total":  2,
  "limit":  50,
  "offset": 0
}
```

### Error Responses

| Status | Condition                              | Body                                           |
|--------|----------------------------------------|------------------------------------------------|
| `400`  | `limit` is negative or non-numeric     | `{"error":"limit must be a non-negative integer"}` |
| `400`  | `offset` is negative or non-numeric    | `{"error":"offset must be a non-negative integer"}` |
| `500`  | Internal server error                  | `{"error":"internal server error"}`            |

### TTL Semantics

`ttl_seconds` in each key entry follows the same conventions as the rest of the backend:

| Value      | Meaning                                             |
|------------|-----------------------------------------------------|
| `-1.0`     | Key is permanent — no TTL set                       |
| `>= 0.0`   | Remaining seconds until the key expires             |

This matches `KeyValueStore::kTtlPermanent = -1.0` defined in `kv_store.h`.

### Ordering

Keys are sorted **lexicographically** by key name (ascending).  
This order is deterministic and stable across calls, which is essential for
consistent pagination behavior.

### Pagination

- `total` always reflects the count of matched keys **before** pagination.
- The client uses `total`, `limit`, and `offset` to compute pagination state.
- Page N: `offset = (N - 1) * limit`
- Requesting `offset >= total` returns `"keys": []` with `"total"` still set correctly.
- `limit` is clamped to a maximum of 100 server-side to prevent memory abuse.

### Prefix Filtering

Prefix filtering is implemented in the HTTP layer after retrieving the snapshot from the store. The backend's `list_keys(now_us)` returns all live keys; the handler then applies `key.compare(0, prefix.size(), prefix) == 0` to filter.

This approach was chosen over a storage-level prefix scan because:
1. The storage layer is a `std::unordered_map` — there is no efficient prefix scan.
2. The total number of live keys is bounded in practice; a full scan is acceptable.
3. It keeps the storage interface minimal and unchanged.

### Expired Keys

`KeyValueStore::list_keys(now_us)` calls `storage_->get_all_with_expiry(now_us)`, which already excludes any key whose `expires_at_us <= now_us`. Logically expired keys never appear in the listing, even if the background cleanup thread has not yet physically removed them.

### Concurrency

The snapshot is taken inside `KeyValueStore::list_keys()` under the existing `std::shared_mutex` (shared lock). Concurrent `PUT /key/:key` and `DELETE /key/:key` requests hold exclusive locks and are therefore serialized with respect to the snapshot.

No second global mutex is introduced in the HTTP layer.

---

## Part B — Frontend API Client

### New Types (`frontend/src/types/api.ts`)

```typescript
/** Single key entry returned by GET /keys */
export interface KeyInfo {
  key: string;
  value: string;
  /** -1 = permanent; ≥ 0 = remaining seconds until expiry */
  ttl_seconds: number;
}

/** Full GET /keys response */
export interface KeyListResponse {
  keys: KeyInfo[];
  total: number;
  limit: number;
  offset: number;
}
```

### New Function (`frontend/src/services/api.ts`)

```typescript
export interface ListKeysOptions {
  prefix?: string;
  limit?: number;
  offset?: number;
}

export function listKeys(options?: ListKeysOptions): Promise<ApiResult<KeyListResponse>>;
```

- `prefix` is omitted from the query string when empty or undefined.
- `offset` is omitted when 0.
- Uses the same `request<T>()` helper as all other API functions — no duplicate fetch logic.

---

## Part C–N — Keys Page UI

### Route

```
/keys
```

URL query parameters (preserved on navigation/refresh):

```
/keys?prefix=user:&page=2
```

### Component Architecture

```
KeysPage
│
├── Toolbar (search form)
│   ├── prefix input
│   ├── Search button
│   └── Clear button (when prefix active)
│
├── Loading state
├── ErrorMessage (with retry)
│
├── Empty state (no keys / no matches)
│
├── Key table
│   └── KeyRow × N
│       ├── Key (monospace, truncated)
│       ├── Value (monospace, truncated to 60 chars)
│       ├── TTL (formatted)
│       └── Actions: Edit | Delete
│
├── Pagination (← Prev | Page X / Y | Next →)
│
├── ViewModal   — read-only key detail
├── CreateModal — create key form
├── EditModal   — edit key form
├── Confirm delete Modal
└── Toast stack
```

### New Files

| File                                               | Description                              |
|----------------------------------------------------|------------------------------------------|
| `frontend/src/pages/KeysPage.tsx`                  | Main Keys page (948 lines)               |
| `frontend/src/pages/KeysPage.module.css`           | Keys page styles (550 lines)             |
| `frontend/src/components/Modal.tsx`                | Accessible modal/dialog                  |
| `frontend/src/components/Modal.module.css`         | Modal styles                             |
| `frontend/src/components/Toast.tsx`                | Toast notifications + `useToast` hook    |
| `frontend/src/components/Toast.module.css`         | Toast styles                             |

### Modified Files

| File                                               | Change                                   |
|----------------------------------------------------|------------------------------------------|
| `frontend/src/types/api.ts`                        | Added `KeyInfo`, `KeyListResponse`       |
| `frontend/src/services/api.ts`                     | Added `listKeys()`, `ListKeysOptions`    |
| `frontend/src/utils/format.ts`                     | Added `formatTtl()`, `truncateValue()`   |
| `frontend/src/layouts/AppLayout.tsx`               | Footer updated to "Stage 16"             |

---

## CRUD Flow

### Browse

1. Page mounts → `listKeys({ limit: 50, offset: 0 })` → populates table.
2. Keys sorted lexicographically by the backend.
3. Each row shows: key, truncated value, TTL badge, Edit/Delete actions.

### Search

1. User types prefix → clicks Search (or presses Enter).
2. URL updated to `/keys?prefix=<value>&page=1`.
3. `listKeys({ prefix, limit: 50, offset: 0 })` called.
4. Result shown. If no match: empty-state message.
5. "Clear" button resets URL and prefix.

### Pagination

1. Backend returns `total`. Frontend computes `totalPages = ceil(total / 50)`.
2. Prev/Next buttons update URL (`?page=N`).
3. Prefix preserved in URL across page changes.
4. Prev disabled on page 1; Next disabled when `page >= totalPages`.

### View Key

1. Click any row → `ViewModal` opens with the full value and TTL.
2. Value rendered in a `<pre>` tag — preserves newlines, never HTML-rendered.
3. "Edit" button in view modal opens `EditModal` for the same key.

### Create Key

1. Click "+ Create Key" → `CreateModal` opens.
2. Fields: Key (required), Value (required), TTL (Permanent | Expires in…).
3. For permanent: no `X-TTL-Seconds` header sent.
4. For expiring: `X-TTL-Seconds: <seconds>` header sent (must be > 0).
5. On success: modal closes, toast shown, list refreshed.
6. On failure: inline error message shown.

### Edit Key

1. Click "Edit" in row or in view modal → `EditModal` opens.
2. Key field is read-only (keys cannot be renamed — delete and recreate).
3. Value pre-filled with freshest value from `GET /key/:key`.
4. TTL pre-filled from listing data.
5. Same semantics as create: omit header for permanent, send for expiring.
6. Uses `PUT /key/:key` (same endpoint as create — full overwrite).

### Delete Key

1. Click "Delete" → confirmation modal with key name.
2. "Delete" confirmed → `DELETE /key/:key`.
3. 404 response (key already expired) is treated as success (graceful).
4. On success: toast shown, list refreshed.

---

## TTL Handling

### Display

`formatTtl(ttl_seconds)` in `format.ts`:

| `ttl_seconds` | Display         |
|---------------|-----------------|
| `< 0`         | `"Permanent"`   |
| `< 1`         | `"Expiring…"`   |
| `>= 1`        | human duration (e.g. `"59m 12s"`) |

TTL badge color:
- Permanent → muted gray
- < 10 seconds remaining → amber (warning)
- ≥ 10 seconds → green

### In Forms

TTL mode selector:
- **Permanent** → `setKey(key, value)` — no `X-TTL-Seconds` header
- **Expires in…** → `setKey(key, value, seconds)` — `X-TTL-Seconds: <seconds>`

`0` and negative values are rejected by the frontend form. The backend also
rejects them with `400`. Neither `0` nor `-1` is sent for permanent intent —
the header is simply omitted, matching the backend's existing `PUT` semantics.

---

## Value Display

- Values may contain newlines, quotes, Unicode, control characters, and JSON.
- Table cells: `truncateValue(value, 60)` replaces `\n` with `↵` and appends `…` if > 60 chars.
- View modal: `<pre>` rendering preserves whitespace and newlines.
- `dangerouslySetInnerHTML` is **never** used.
- All key/value content flows through normal React text rendering.

---

## URL / Route State

The active prefix filter and page number are stored in URL query params:

```
/keys?prefix=user:&page=2
```

This means:
- Refreshing the browser restores the current view.
- Browser back/forward navigate pagination history.
- Sharing the URL shows the same page.

The `useSearchParams` hook from React Router is used to sync URL ↔ state.

---

## Error Handling

| Scenario              | UI Response                                                    |
|-----------------------|----------------------------------------------------------------|
| Network failure       | `ErrorMessage` with "Try again" retry button                   |
| `400` (bad request)   | Inline form error or toast                                     |
| `404` on delete       | Treated as success (key already gone — expired or race)        |
| `500` on list         | `ErrorMessage` with retry                                      |
| `500` on create/edit  | Inline form error + error toast                                |

Raw stack traces are never shown to the user.

---

## Responsive Design

| Screen width | Behavior                                      |
|--------------|-----------------------------------------------|
| Desktop      | Full 4-column table                           |
| Tablet       | Full 4-column table (horizontal scroll)       |
| Mobile       | Value + TTL columns hidden; Key + Actions visible. Toolbar stacks vertically. |

The `tableWrap` div uses `overflow-x: auto` for horizontal scroll on narrow screens.

---

## Accessibility

- Semantic headings (`h1`, `h2`).
- `<form role="search">` with `aria-label` for the search toolbar.
- All inputs have associated `<label>` elements.
- Modal uses `role="dialog"`, `aria-modal="true"`, `aria-labelledby="modal-title"`.
- Escape key closes any open modal.
- Focus moves to the modal panel when it opens (`tabIndex={-1}` + `.focus()`).
- Buttons have `aria-label` where the visible text is ambiguous (icon-only).
- Toast container uses `aria-live="polite"` so screen readers announce new messages.
- Delete confirmation uses `aria-label` on the confirm button including the key name.
- All colors have text alternatives (no color-only status indicators).
- `:focus-visible` ring defined in global CSS.

---

## Known Limitations

1. **Value size**: Very large values (megabytes) are fetched in full for the view and edit modals. There is no streaming or truncation at the HTTP layer.

2. **No real-time updates**: The list does not auto-refresh. TTL countdown is a snapshot from the last refresh. Users must click Refresh to see current state.

3. **Prefix filter is client-side sorted, server-side scanned**: Because `std::unordered_map` has no ordered prefix scan, the server always retrieves all live keys and filters in memory. This is acceptable for current scale but would need a different data structure for millions of keys.

4. **Keys cannot be renamed**: `PUT /key/:key` is a full overwrite. To rename, delete the old key and create a new one.

5. **No multi-select / bulk delete**: Each key must be deleted individually.

6. **TTL display is a snapshot**: The TTL shown in the table reflects the state at the last fetch. As time passes, the displayed TTL becomes stale. Click Refresh to re-fetch current TTLs.

7. **Mobile Value column hidden**: On screens narrower than 640 px, the Value and TTL columns are hidden to keep the table usable. The full value is accessible via the View modal.

---

## Backend Files Changed

| File                                | Change                                             |
|-------------------------------------|----------------------------------------------------|
| `include/forgekv/kv_store.h`        | Added `KeyInfo` struct + `list_keys()` declaration |
| `include/forgekv/http_server.h`     | Added `json_keys_response()` declaration           |
| `src/kv_store.cpp`                  | Implemented `list_keys()`                          |
| `src/http_server.cpp`               | Registered `GET /keys` handler; added `json_keys_response()` |
| `tests/test_http_keys.cpp`          | 21 new integration tests                           |
| `CMakeLists.txt`                    | Added `forgekv_tests_http_keys` target             |

## Test Results

```
13/13 test targets pass
Tests: 21 new (GET /keys) + all existing 462 tests
```
