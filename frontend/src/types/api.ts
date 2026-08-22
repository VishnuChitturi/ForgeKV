// =============================================================================
// ForgeKV API types
//
// These types mirror the exact JSON shapes produced by the C++ backend.
// Do not add fields that the backend does not return.
// =============================================================================

// ---------------------------------------------------------------------------
// GET /health → 200
// ---------------------------------------------------------------------------
export interface HealthResponse {
  status: "ok";
}

// ---------------------------------------------------------------------------
// GET /stats → 200
//
// All integer fields are uint64 on the backend; JavaScript receives them as
// numbers (safe as long as values stay within Number.MAX_SAFE_INTEGER).
// uptime_seconds is a double serialized to 2 decimal places.
// last_snapshot_time_us is microseconds since Unix epoch; 0 means never.
// ---------------------------------------------------------------------------
export interface StatsResponse {
  key_count: number;
  get_hits: number;
  get_misses: number;
  set_count: number;
  delete_count: number;
  ttl_set_count: number;
  expired_count: number;
  wal_size_bytes: number;
  uptime_seconds: number;
  last_snapshot_time_us: number;
}

// ---------------------------------------------------------------------------
// GET /key/:key → 200 when found
// ---------------------------------------------------------------------------
export interface GetKeyResponse {
  key: string;
  value: string;
}

// ---------------------------------------------------------------------------
// PUT /key/:key → 200
// DELETE /key/:key → 200
// ---------------------------------------------------------------------------
export interface StatusResponse {
  status: "ok";
}

// Convenience aliases for specific endpoints
export type SetKeyResponse = StatusResponse;
export type DeleteKeyResponse = StatusResponse;

// ---------------------------------------------------------------------------
// Any 4xx / 5xx error response
// ---------------------------------------------------------------------------
export interface ErrorResponse {
  error: string;
}

// ---------------------------------------------------------------------------
// GET /keys → 200   (Stage 16)
//
// ttl_seconds semantics (matching backend KeyValueStore::kTtlPermanent):
//   -1.0   → permanent key (no TTL)
//   >= 0.0 → remaining seconds until expiry
// ---------------------------------------------------------------------------
export interface KeyInfo {
  key: string;
  value: string;
  /** -1 = permanent; ≥ 0 = remaining seconds */
  ttl_seconds: number;
}

export interface KeyListResponse {
  keys: KeyInfo[];
  /** Total matched keys before pagination */
  total: number;
  /** Effective page size used */
  limit: number;
  /** Effective offset used */
  offset: number;
}

// ---------------------------------------------------------------------------
// Discriminated union for callers that need to distinguish success/failure
// ---------------------------------------------------------------------------
export type ApiResult<T> =
  | { ok: true; data: T }
  | { ok: false; status: number; error: string };
