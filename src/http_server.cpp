// =============================================================================
// ForgeKV — Stage 17: HttpServer implementation (Admin Maintenance API)
// =============================================================================
//
// See include/forgekv/http_server.h for the full design and API documentation.
//
// This file implements:
//
//   1. HttpServer constructor — calls register_routes(). No InlineTaskQueue.
//   2. listen() / stop() / bind_to_any_port() / listen_after_bind() / wait_until_ready()
//      — thin wrappers around httplib::Server.
//   3. register_routes() — registers all REST endpoint handlers on server_.
//   4. json_escape() — correct JSON string escaping.
//   5. json_status() / json_error() / json_kv() — response serialization helpers.
//   6. json_keys_response() — GET /keys response serialization.
//
// GET /keys IMPLEMENTATION NOTES (Stage 16)
// ------------------------------------------
// Query parameters:
//   prefix  (string, default "")  — return only keys with this prefix
//   limit   (integer, default 50, max 100) — max results per page
//   offset  (integer, default 0)  — skip this many results
//
// Algorithm:
//   1. Parse + validate query params; return 400 on error.
//   2. Call store_.get_all_with_expiry_snapshot() via the TTL-aware storage
//      path (through KeyValueStore's public ttl() + get() APIs operating
//      under the shared lock), or more precisely:
//      We call a new KeyValueStore::list_keys() method that acquires the
//      shared lock once, snapshots all live (non-expired) entries, applies
//      prefix filter, sorts lexicographically, computes TTL metadata,
//      and returns the full filtered+sorted list. Pagination is applied
//      in the HTTP layer after receiving the snapshot.
//   3. Sort lexicographically (deterministic for pagination).
//   4. Apply offset+limit to produce the response page.
//   5. Serialize to JSON.
//
// Concurrency:
//   KeyValueStore::list_keys() acquires the shared lock once for the snapshot.
//   Concurrent GET /key, PUT /key, DELETE /key are safe.
//
// CONCURRENT OPERATION
// --------------------------------
// Stage 7+ uses cpp-httplib's default ThreadPool. Thread safety provided by
// KeyValueStore's std::shared_mutex.
// =============================================================================

#include "forgekv/http_server.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace forgekv {

// =============================================================================
// Constructor
// =============================================================================
//
// Store the reference to the KV engine and register all route handlers.
// No task queue override — cpp-httplib's default ThreadPool is used.

HttpServer::HttpServer(KeyValueStore& store)
    : store_(store)
{
    // Stage 7: Do NOT override new_task_queue. cpp-httplib's default ThreadPool
    // allows concurrent request handling. KeyValueStore's shared_mutex ensures
    // thread safety for all store operations.
    register_routes();
}

// =============================================================================
// listen
// =============================================================================
//
// Blocks until stop() is called or the server encounters an unrecoverable
// error. Returns false if the server could not bind to the given address.

bool HttpServer::listen(const std::string& host, int port) {
    return server_.listen(host.c_str(), port);
}

// =============================================================================
// bind_to_any_port
// =============================================================================
//
// Binds to an OS-assigned ephemeral port on host.
// Returns the bound port number, or -1 on failure.

int HttpServer::bind_to_any_port(const std::string& host) {
    return server_.bind_to_any_port(host);
}

// =============================================================================
// listen_after_bind
// =============================================================================
//
// Begins accepting on the socket bound by bind_to_any_port().
// Blocks until stop() is called.

bool HttpServer::listen_after_bind() {
    return server_.listen_after_bind();
}

// =============================================================================
// wait_until_ready
// =============================================================================
//
// Blocks until the server's is_running_ flag is true.
// Call from the test thread after launching listen_after_bind() on a
// background thread to ensure the server is accepting before the first
// client request.

void HttpServer::wait_until_ready() const {
    server_.wait_until_ready();
}

// =============================================================================
// stop
// =============================================================================

void HttpServer::stop() {
    server_.stop();
}

// =============================================================================
// register_routes
// =============================================================================
//
// Registers the four REST endpoints. Each lambda captures `this` and
// delegates to the appropriate handler logic.
//
// Path parameter syntax: /key/:key
//   cpp-httplib extracts the segment after /key/ and makes it available via
//   req.path_params.at("key").
//
// NOTE: Full endpoint business logic is NOT implemented here yet.
//   This checkpoint (Stage 6A) establishes the routing skeleton only.
//   Endpoint bodies will be filled in Stage 6B.

void HttpServer::register_routes() {
    // =========================================================================
    // CORS support
    // =========================================================================
    //
    // Two mechanisms work together:
    //
    // 1. set_post_routing_handler — runs after every dispatched request
    //    (GET, PUT, DELETE, POST, OPTIONS) and before the response is written.
    //    Injects Access-Control-Allow-Origin: * on every response so that
    //    simple cross-origin requests (GET /health, GET /stats, etc.) succeed.
    //
    // 2. Options(".*") — catches all OPTIONS preflight requests regardless of
    //    path. Returns 204 No Content with the full set of preflight headers.
    //    The pattern ".*" has no "/:" so cpp-httplib treats it as a regex
    //    matching any path.
    //
    // Allowed methods: GET, PUT, DELETE, POST, OPTIONS
    // Allowed headers: Content-Type, X-TTL-Seconds
    // Max-Age: 86400 (24 hours — browsers cache the preflight result)

    server_.set_post_routing_handler(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin",  "*");
            res.set_header("Access-Control-Allow-Methods",
                           "GET, PUT, DELETE, POST, OPTIONS");
            res.set_header("Access-Control-Allow-Headers",
                           "Content-Type, X-TTL-Seconds");
        });

    // OPTIONS preflight handler — catches every path.
    // Returns 204 No Content; the post_routing_handler above then appends
    // the Allow-Origin / Allow-Methods / Allow-Headers headers.
    server_.Options(".*", [](const httplib::Request& /*req*/,
                              httplib::Response& res) {
        res.status = 204;
        res.set_header("Access-Control-Max-Age", "86400");
    });

    // -------------------------------------------------------------------------
    // GET /key/:key — retrieve the value for a key
    // -------------------------------------------------------------------------
    //
    // Returns 200 + {"key":"...","value":"..."} if the key exists.
    // Returns 404 + {"error":"key not found"} if the key does not exist.
    // Returns 500 on unexpected exception.
    server_.Get("/key/:key", [this](const httplib::Request& req,
                                   httplib::Response&       res) {
        try {
            const std::string& key = req.path_params.at("key");
            auto opt = store_.get(key);
            if (opt.has_value()) {
                res.status = 200;
                res.set_content(json_kv(key, opt.value()), "application/json");
            } else {
                res.status = 404;
                res.set_content(json_error("key not found"), "application/json");
            }
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(json_error("internal server error"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // PUT /key/:key — store or update a value
    // -------------------------------------------------------------------------
    //
    // The raw request body is used as the value.
    // Returns 400 if the body is empty.
    // Returns 200 + {"status":"ok"} on success.
    // Returns 500 on unexpected exception.
    //
    // Stage 10: Optional X-TTL-Seconds header.
    //   If present and valid (integer or decimal > 0):
    //     → store key with that TTL (set_with_ttl)
    //   If absent:
    //     → store permanently (set)
    //   If present but invalid (non-numeric, <= 0):
    //     → 400 Bad Request
    server_.Put("/key/:key", [this](const httplib::Request& req,
                                   httplib::Response&       res) {
        try {
            const std::string& key   = req.path_params.at("key");
            const std::string& value = req.body;

            if (value.empty()) {
                res.status = 400;
                res.set_content(json_error("value cannot be empty"),
                                "application/json");
                return;
            }

            // Check for optional X-TTL-Seconds header.
            if (req.has_header("X-TTL-Seconds")) {
                const std::string& ttl_str = req.get_header_value("X-TTL-Seconds");
                double ttl_val = 0.0;
                try {
                    std::size_t pos = 0;
                    ttl_val = std::stod(ttl_str, &pos);
                    // Ensure the entire header value was consumed (no trailing junk).
                    if (pos != ttl_str.size()) {
                        throw std::invalid_argument("trailing characters");
                    }
                } catch (const std::exception&) {
                    res.status = 400;
                    res.set_content(
                        json_error("X-TTL-Seconds must be a positive number"),
                        "application/json");
                    return;
                }

                if (ttl_val <= 0.0) {
                    res.status = 400;
                    res.set_content(
                        json_error("X-TTL-Seconds must be greater than 0"),
                        "application/json");
                    return;
                }

                store_.set_with_ttl(key, value, ttl_val);
            } else {
                store_.set(key, value);
            }

            res.status = 200;
            res.set_content(json_status("ok"), "application/json");
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(json_error("internal server error"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // DELETE /key/:key — remove a key from the store
    // -------------------------------------------------------------------------
    //
    // Returns 200 + {"status":"ok"} if the key existed and was deleted.
    // Returns 404 + {"error":"key not found"} if the key did not exist.
    // Returns 500 on unexpected exception.
    server_.Delete("/key/:key", [this](const httplib::Request& req,
                                      httplib::Response&       res) {
        try {
            const std::string& key = req.path_params.at("key");
            bool deleted = store_.del(key);
            if (deleted) {
                res.status = 200;
                res.set_content(json_status("ok"), "application/json");
            } else {
                res.status = 404;
                res.set_content(json_error("key not found"), "application/json");
            }
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(json_error("internal server error"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /health — server health check
    // -------------------------------------------------------------------------
    //
    // Always returns 200 + {"status":"ok"}.
    // Does not access or modify the key-value store.
    server_.Get("/health", [](const httplib::Request& /*req*/,
                              httplib::Response&       res) {
        res.status = 200;
        res.set_content("{\"status\":\"ok\"}", "application/json");
    });

    // -------------------------------------------------------------------------
    // GET /stats — runtime statistics
    // -------------------------------------------------------------------------
    //
    // Stage 11: returns a JSON object with current operational metrics.
    //
    // Response shape:
    //   {
    //     "key_count":             <uint64>,
    //     "get_hits":              <uint64>,
    //     "get_misses":            <uint64>,
    //     "set_count":             <uint64>,
    //     "delete_count":          <uint64>,
    //     "ttl_set_count":         <uint64>,
    //     "expired_count":         <uint64>,
    //     "wal_size_bytes":        <uint64>,
    //     "uptime_seconds":        <double, 2 decimal places>,
    //     "last_snapshot_time_us": <uint64, 0 means never>
    //   }
    //
    // Always returns HTTP 200. Internal errors return 500.
    // No mutation is performed.
    server_.Get("/stats", [this](const httplib::Request& /*req*/,
                                 httplib::Response&       res) {
        try {
            const forgekv::Stats s = store_.stats();

            // Serialize to JSON manually (consistent with other endpoints).
            std::ostringstream oss;
            oss << "{"
                << "\"key_count\":"              << s.key_count             << ","
                << "\"get_hits\":"               << s.get_hits              << ","
                << "\"get_misses\":"             << s.get_misses            << ","
                << "\"set_count\":"              << s.set_count             << ","
                << "\"delete_count\":"           << s.delete_count          << ","
                << "\"ttl_set_count\":"          << s.ttl_set_count         << ","
                << "\"expired_count\":"          << s.expired_count         << ","
                << "\"wal_size_bytes\":"         << s.wal_size_bytes        << ","
                << "\"uptime_seconds\":"         << std::fixed
                                                 << std::setprecision(2)
                                                 << s.uptime_seconds        << ","
                << "\"last_snapshot_time_us\":"  << s.last_snapshot_time_us
                << "}";

            res.status = 200;
            res.set_content(oss.str(), "application/json");
        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(json_error("internal server error"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // GET /keys — list all live keys (Stage 16)
    // -------------------------------------------------------------------------
    //
    // Query parameters:
    //   prefix  (string, default "")  — filter keys by prefix
    //   limit   (integer, default 50, max 100) — page size
    //   offset  (integer, default 0)  — skip N results
    //
    // Response (200):
    //   {
    //     "keys": [
    //       { "key": "...", "value": "...", "ttl_seconds": -1.0 },
    //       ...
    //     ],
    //     "total": <N>,    -- total matched keys (after prefix filter, before pagination)
    //     "limit": <L>,    -- effective limit used
    //     "offset": <O>    -- effective offset used
    //   }
    //
    // Returns 400 for invalid limit / offset.
    // Returns 500 on internal error.
    //
    // Keys are sorted lexicographically (deterministic for pagination).
    // Only live (non-expired) keys are included.
    // The snapshot is taken under the shared lock once; TTL is computed from
    // the same timestamp to keep the view consistent.
    server_.Get("/keys", [this](const httplib::Request& req,
                                httplib::Response&       res) {
        // --- Parse query parameters ---

        // prefix: optional, default ""
        std::string prefix;
        if (req.has_param("prefix")) {
            prefix = req.get_param_value("prefix");
        }

        // limit: optional, default 50, max 100
        static constexpr std::size_t kDefaultLimit = 50;
        static constexpr std::size_t kMaxLimit      = 100;
        std::size_t limit = kDefaultLimit;
        if (req.has_param("limit")) {
            const std::string& ls = req.get_param_value("limit");
            try {
                long long val = 0;
                std::size_t pos = 0;
                val = std::stoll(ls, &pos);
                if (pos != ls.size()) {
                    throw std::invalid_argument("trailing characters");
                }
                if (val < 0) {
                    res.status = 400;
                    res.set_content(json_error("limit must be >= 0"), "application/json");
                    return;
                }
                limit = static_cast<std::size_t>(
                    std::min(static_cast<long long>(kMaxLimit), val));
            } catch (const std::exception&) {
                res.status = 400;
                res.set_content(json_error("limit must be a non-negative integer"),
                                "application/json");
                return;
            }
        }

        // offset: optional, default 0
        std::size_t offset = 0;
        if (req.has_param("offset")) {
            const std::string& os = req.get_param_value("offset");
            try {
                long long val = 0;
                std::size_t pos = 0;
                val = std::stoll(os, &pos);
                if (pos != os.size()) {
                    throw std::invalid_argument("trailing characters");
                }
                if (val < 0) {
                    res.status = 400;
                    res.set_content(json_error("offset must be >= 0"), "application/json");
                    return;
                }
                offset = static_cast<std::size_t>(val);
            } catch (const std::exception&) {
                res.status = 400;
                res.set_content(json_error("offset must be a non-negative integer"),
                                "application/json");
                return;
            }
        }

        try {
            // Capture "now" once so all TTL computations in this request are
            // consistent with the same clock snapshot.
            const auto now_tp  = std::chrono::system_clock::now();
            const auto now_us  = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    now_tp.time_since_epoch()).count());

            // Ask the store for all live entries (under its shared lock).
            // list_keys(now_us) excludes already-expired keys and computes
            // remaining TTL for each entry. Returns KeyValueStore::KeyInfo.
            auto raw = store_.list_keys(now_us);

            // Sort lexicographically for deterministic pagination.
            std::sort(raw.begin(), raw.end(),
                      [](const forgekv::KeyValueStore::KeyInfo& a,
                         const forgekv::KeyValueStore::KeyInfo& b) {
                          return a.key < b.key;
                      });

            // Apply prefix filter.
            std::vector<forgekv::KeyValueStore::KeyInfo> filtered;
            filtered.reserve(raw.size());
            for (auto& e : raw) {
                if (prefix.empty() ||
                    (e.key.size() >= prefix.size() &&
                     e.key.compare(0, prefix.size(), prefix) == 0)) {
                    filtered.push_back(std::move(e));
                }
            }

            const std::size_t total = filtered.size();

            // Apply pagination.
            std::vector<forgekv::KeyValueStore::KeyInfo> page;
            if (offset < total) {
                const std::size_t end = std::min(offset + limit, total);
                page.assign(
                    std::make_move_iterator(filtered.begin() +
                                            static_cast<std::ptrdiff_t>(offset)),
                    std::make_move_iterator(filtered.begin() +
                                            static_cast<std::ptrdiff_t>(end)));
            }

            res.status = 200;
            res.set_content(
                json_keys_response(page, total, limit, offset),
                "application/json");

        } catch (const std::exception&) {
            res.status = 500;
            res.set_content(json_error("internal server error"), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // POST /snapshot — trigger an immediate snapshot  (Stage 17)
    // -------------------------------------------------------------------------
    //
    // Calls KeyValueStore::snapshot() which uses the existing locking semantics
    // (exclusive write lock inside snapshot()). No additional HTTP-level
    // locking is introduced here.
    //
    // Returns 200 + {"status":"ok"}      on success.
    // Returns 500 + {"error":"..."}      if snapshot() returns false or throws.
    //
    // After success the client should call GET /stats to see the updated
    // last_snapshot_time_us value.
    server_.Post("/snapshot", [this](const httplib::Request& /*req*/,
                                     httplib::Response&       res) {
        try {
            const bool ok = store_.snapshot();
            if (ok) {
                res.status = 200;
                res.set_content(json_status("ok"), "application/json");
            } else {
                res.status = 500;
                res.set_content(json_error("snapshot failed"), "application/json");
            }
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json_error(e.what()), "application/json");
        }
    });

    // -------------------------------------------------------------------------
    // POST /compact — trigger WAL compaction  (Stage 17)
    // -------------------------------------------------------------------------
    //
    // Calls KeyValueStore::compact() which rewrites the WAL to contain only
    // the current live state. Uses the existing compaction locking semantics
    // (exclusive write lock inside compact()). No additional HTTP-level
    // locking is introduced here.
    //
    // Returns 200 + {"status":"ok"}      on completion.
    // Returns 500 + {"error":"..."}      on exception.
    //
    // Note: compact() does not return a bool — it throws on failure.
    // All state-invariant guarantees are provided by KeyValueStore::compact().
    server_.Post("/compact", [this](const httplib::Request& /*req*/,
                                    httplib::Response&       res) {
        try {
            store_.compact();
            res.status = 200;
            res.set_content(json_status("ok"), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(json_error(e.what()), "application/json");
        }
    });
}

// =============================================================================
// json_escape
// =============================================================================
//
// Produces a JSON-safe version of s suitable for embedding between double
// quotes in a JSON string. Does NOT include the surrounding quotes.
//
// Characters escaped:
//   "   → \"
//   \   → \\
//   \b  → \b  (U+0008 backspace)
//   \f  → \f  (U+000C form feed)
//   \n  → \n  (U+000A newline)
//   \r  → \r  (U+000D carriage return)
//   \t  → \t  (U+0009 tab)
//   any other control character (0x00–0x1F) → \uXXXX (4-hex uppercase)
//
// Characters not escaped:
//   All printable ASCII (0x20–0x7E) except " and \.
//   High bytes (0x80–0xFF) are passed through as-is; the API contract states
//   that keys and values are treated as text (not arbitrary binary data).

std::string HttpServer::json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size()); // minimum allocation; may grow for escapes

    for (const unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20u) {
                    // Other control characters → \uXXXX
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04X",
                                  static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

// =============================================================================
// json_status
// =============================================================================
//
// Returns: {"status":"<status>"}

std::string HttpServer::json_status(const std::string& status) {
    return "{\"status\":\"" + json_escape(status) + "\"}";
}

// =============================================================================
// json_error
// =============================================================================
//
// Returns: {"error":"<message>"}

std::string HttpServer::json_error(const std::string& message) {
    return "{\"error\":\"" + json_escape(message) + "\"}";
}

// =============================================================================
// json_kv
// =============================================================================
//
// Returns: {"key":"<key>","value":"<value>"}

std::string HttpServer::json_kv(const std::string& key,
                                const std::string& value) {
    return "{\"key\":\""   + json_escape(key)
         + "\",\"value\":\"" + json_escape(value)
         + "\"}";
}

// =============================================================================
// json_keys_response  (Stage 16)
// =============================================================================
//
// Serializes the GET /keys response.
//
// Shape:
// {
//   "keys": [
//     { "key": "<k>", "value": "<v>", "ttl_seconds": <d> },
//     ...
//   ],
//   "total":  <N>,
//   "limit":  <L>,
//   "offset": <O>
// }
//
// ttl_seconds conventions (matching KeyValueStore::ttl()):
//   -1.0  → permanent (kTtlPermanent)
//   ≥ 0.0 → remaining seconds until expiry

std::string HttpServer::json_keys_response(
    const std::vector<KeyValueStore::KeyInfo>& entries,
    std::size_t total,
    std::size_t limit,
    std::size_t offset)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3);

    oss << "{\"keys\":[";
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) oss << ',';
        const auto& e = entries[i];
        oss << "{"
            << "\"key\":\""         << json_escape(e.key)   << "\","
            << "\"value\":\""       << json_escape(e.value) << "\","
            << "\"ttl_seconds\":"   << e.ttl_seconds
            << "}";
    }
    oss << "],"
        << "\"total\":"  << total  << ","
        << "\"limit\":"  << limit  << ","
        << "\"offset\":" << offset
        << "}";

    return oss.str();
}

} // namespace forgekv
