// =============================================================================
// ForgeKV — Stage 7: HttpServer implementation (Concurrent Request Handling)
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
//
// CONCURRENT OPERATION (Stage 7)
// --------------------------------
// Stage 6 installed an InlineTaskQueue that ran every request handler
// synchronously on the accept-loop thread — fully single-threaded.
//
// Stage 7 removes that override entirely. cpp-httplib's default ThreadPool
// task queue is used, which dispatches each accepted connection to a worker
// thread. Multiple requests can now execute concurrently.
//
// Thread safety for the KeyValueStore is provided by its std::shared_mutex:
//   - GET /key/:key and GET /health call read-only operations → shared locks.
//   - PUT /key/:key calls store_.set() → exclusive lock.
//   - DELETE /key/:key calls store_.del() → exclusive lock.
//
// ROUTE STRUCTURE
// ---------------
// All four endpoints are registered in register_routes() with full business
// logic implemented:
//
//   GET  /key/:key  — 200+JSON if found, 404 if not found
//   PUT  /key/:key  — 400 if body empty, 200 on success
//   DELETE /key/:key — 200 if deleted, 404 if not found
//   GET  /health    — always 200 {"status":"ok"}
//
// CONTENT TYPE
// ------------
// All responses use Content-Type: application/json.
// PUT request bodies are read as plain text (the raw body is the value).
//
// JSON ESCAPING
// -------------
// json_escape() handles:
//   "  → \"
//   \  → \\
//   \n → \n
//   \r → \r
//   \t → \t
//   control characters 0x00–0x1F → \uXXXX
//
// Keys and values may contain any of these characters.
// =============================================================================

#include "forgekv/http_server.h"

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

} // namespace forgekv
