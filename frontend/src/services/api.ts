// =============================================================================
// ForgeKV API client
//
// Single, centralized module for communicating with the ForgeKV HTTP server.
//
// In development the Vite server proxies /api/* → VITE_API_BASE_URL so the
// browser never makes a cross-origin request and CORS is not needed.
//
// In production builds, set VITE_API_BASE_URL to the backend origin and
// ensure the server accepts requests from the frontend origin, or serve
// the frontend and backend from the same origin.
//
// All functions return ApiResult<T> — callers do not need try/catch.
// Network or JSON-parse failures are caught internally and surfaced as
// { ok: false, status: 0, error: "<message>" }.
// =============================================================================

import type {
  ApiResult,
  DeleteKeyResponse,
  GetKeyResponse,
  HealthResponse,
  SetKeyResponse,
  StatsResponse,
} from "../types/api";

// ---------------------------------------------------------------------------
// Base path
//
// All API calls go through /api which the Vite dev proxy rewrites to the
// backend root. In production, set VITE_API_BASE_URL appropriately.
// ---------------------------------------------------------------------------
const API_PREFIX = "/api";

// ---------------------------------------------------------------------------
// Internal helper: perform a fetch and normalise into ApiResult<T>
// ---------------------------------------------------------------------------
async function request<T>(
  path: string,
  init?: RequestInit
): Promise<ApiResult<T>> {
  try {
    const res = await fetch(`${API_PREFIX}${path}`, {
      headers: { Accept: "application/json", ...init?.headers },
      ...init,
    });

    const text = await res.text();

    if (!res.ok) {
      // Attempt to parse backend error JSON { "error": "..." }
      try {
        const json = JSON.parse(text) as { error?: string };
        return {
          ok: false,
          status: res.status,
          error: json.error ?? `HTTP ${res.status}`,
        };
      } catch {
        return { ok: false, status: res.status, error: `HTTP ${res.status}` };
      }
    }

    try {
      const data = JSON.parse(text) as T;
      return { ok: true, data };
    } catch {
      return {
        ok: false,
        status: res.status,
        error: "Unexpected non-JSON response from server",
      };
    }
  } catch (err) {
    const message =
      err instanceof Error ? err.message : "Network request failed";
    return { ok: false, status: 0, error: message };
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * GET /health
 * Returns { status: "ok" } when the server is reachable and healthy.
 */
export function getHealth(): Promise<ApiResult<HealthResponse>> {
  return request<HealthResponse>("/health");
}

/**
 * GET /stats
 * Returns current operational metrics from the ForgeKV engine.
 */
export function getStats(): Promise<ApiResult<StatsResponse>> {
  return request<StatsResponse>("/stats");
}

/**
 * GET /key/:key
 * Returns { key, value } if the key exists; 404 error result otherwise.
 */
export function getKey(key: string): Promise<ApiResult<GetKeyResponse>> {
  return request<GetKeyResponse>(`/key/${encodeURIComponent(key)}`);
}

/**
 * PUT /key/:key
 * Stores value for key. Optionally sets a TTL in seconds (X-TTL-Seconds).
 * Returns { status: "ok" } on success.
 */
export function setKey(
  key: string,
  value: string,
  ttlSeconds?: number
): Promise<ApiResult<SetKeyResponse>> {
  const headers: Record<string, string> = {
    "Content-Type": "text/plain",
  };
  if (ttlSeconds !== undefined && ttlSeconds > 0) {
    headers["X-TTL-Seconds"] = String(ttlSeconds);
  }
  return request<SetKeyResponse>(`/key/${encodeURIComponent(key)}`, {
    method: "PUT",
    headers,
    body: value,
  });
}

/**
 * DELETE /key/:key
 * Deletes a key. Returns { status: "ok" } if deleted; 404 otherwise.
 */
export function deleteKey(key: string): Promise<ApiResult<DeleteKeyResponse>> {
  return request<DeleteKeyResponse>(`/key/${encodeURIComponent(key)}`, {
    method: "DELETE",
  });
}
