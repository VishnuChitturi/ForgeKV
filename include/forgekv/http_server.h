#pragma once

// =============================================================================
// ForgeKV — Stage 7: HttpServer (Concurrent Request Handling)
// =============================================================================
//
// HttpServer is a thin HTTP translation layer on top of the ForgeKV engine.
// It accepts HTTP requests from clients, routes them to the correct
// KeyValueStore operation, and returns structured JSON responses.
//
// This class does NOT contain storage logic. It is a pure adapter between
// the HTTP protocol and the KeyValueStore API.
//
// Architecture:
//
//   HTTP Client(s)
//       │
//       ▼
//   HttpServer          ← this class (Stage 6 API, Stage 7 concurrency)
//       │
//       ▼
//   KeyValueStore       ← thread-safe as of Stage 7 (shared_mutex)
//       │
//       ▼
//   WAL + InMemoryStorage
//
// REST API (unchanged from Stage 6):
//
//   GET    /key/<key>   → 200 {"key":"...","value":"..."}
//                         404 {"error":"key not found"}
//
//   PUT    /key/<key>   → 200 {"status":"ok"}
//         body: plain-text value
//
//   DELETE /key/<key>   → 200 {"status":"ok"}
//                         404 {"error":"key not found"}
//
//   GET    /health      → 200 {"status":"ok"}
//
// Concurrent operation (Stage 7):
//
//   Stage 6 used an InlineTaskQueue that ran every request handler directly
//   on the accept-loop thread, making the server fully single-threaded.
//
//   Stage 7 removes that constraint. The server now uses cpp-httplib's
//   default ThreadPool task queue. Each incoming connection is dispatched to
//   a worker thread from the pool, allowing multiple requests to be handled
//   concurrently.
//
//   Thread safety is guaranteed by KeyValueStore's std::shared_mutex:
//     - Concurrent GET/health requests acquire shared locks (readers).
//     - Concurrent PUT/DELETE requests acquire exclusive locks (writers).
//
// JSON serialization:
//
//   Responses use hand-serialized JSON. A private json_escape() helper
//   correctly escapes ", \, \n, \r, \t, and all other control characters
//   in keys and values. No external JSON library is used.
//
// HTTP library:
//
//   cpp-httplib v0.18.5, vendored at third_party/httplib/httplib.h.
//   MIT license. Single-header, no HTTPS/OpenSSL required.
//
// Thread safety: THREAD-SAFE as of Stage 7 (via KeyValueStore's shared_mutex).
// =============================================================================

#include "forgekv/kv_store.h"

// cpp-httplib — vendored single-header library
#include "httplib.h"

#include <string>

namespace forgekv {

// =============================================================================
// HttpServer
// =============================================================================

class HttpServer {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Takes a non-owning reference to a KeyValueStore.
    // The store must outlive the HttpServer.
    //
    // On construction:
    //   1. All route handlers are registered.
    //   2. cpp-httplib's default ThreadPool task queue is used — no override.
    //   3. The server is NOT yet listening — call listen() to start.
    explicit HttpServer(KeyValueStore& store);

    // Not copyable — owns an httplib::Server which is non-copyable.
    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Not movable — route handlers capture `this`; moving would invalidate
    // those captures.
    HttpServer(HttpServer&&)            = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    ~HttpServer() = default;

    // -------------------------------------------------------------------------
    // Listen
    // -------------------------------------------------------------------------

    // Start listening on the given host and port.
    //
    // This call BLOCKS until stop() is called or an error occurs.
    //
    // host:  e.g. "0.0.0.0" to listen on all interfaces,
    //             "127.0.0.1" for loopback only.
    // port:  TCP port number, e.g. 8080.
    //        Pass 0 to let the OS assign an ephemeral port (useful in tests).
    //
    // Returns true if the server ran and shut down cleanly.
    // Returns false if the server failed to bind or encountered an error.
    bool listen(const std::string& host, int port);

    // -------------------------------------------------------------------------
    // Ephemeral-port listen (for testing)
    // -------------------------------------------------------------------------

    // Bind to an OS-assigned ephemeral port on host.
    // Returns the bound port number, or -1 on failure.
    // After a successful call, call listen_after_bind() to begin accepting.
    int bind_to_any_port(const std::string& host);

    // Begin accepting connections on the socket bound by bind_to_any_port().
    // BLOCKS until stop() is called.
    // Returns false if the server was not bound or an error occurs.
    bool listen_after_bind();

    // Block until the server is ready to accept connections.
    // Call this from the test thread after launching listen_after_bind() on
    // a background thread.
    void wait_until_ready() const;

    // Stop the server. Safe to call from a signal handler or another thread
    // (cpp-httplib's stop() is designed for this).
    void stop();

private:
    // -------------------------------------------------------------------------
    // Route registration
    // -------------------------------------------------------------------------

    // Register all REST endpoint handlers on server_.
    // Called once from the constructor.
    void register_routes();

    // -------------------------------------------------------------------------
    // JSON helpers
    // -------------------------------------------------------------------------

    // Escape a string for safe embedding inside a JSON string value.
    // Escapes: " → \", \ → \\, \n → \n, \r → \r, \t → \t,
    //          and all other control characters (0x00–0x1F) as \uXXXX.
    static std::string json_escape(const std::string& s);

    // Build a JSON object with a single "status" field.
    // Example: json_status("ok") → {"status":"ok"}
    static std::string json_status(const std::string& status);

    // Build a JSON object with a single "error" field.
    // Example: json_error("key not found") → {"error":"key not found"}
    static std::string json_error(const std::string& message);

    // Build a JSON object with "key" and "value" fields.
    // Example: json_kv("foo", "bar") → {"key":"foo","value":"bar"}
    static std::string json_kv(const std::string& key,
                               const std::string& value);

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    KeyValueStore&  store_;   // non-owning reference to the KV engine
    httplib::Server server_;  // the underlying HTTP server instance
};

} // namespace forgekv
