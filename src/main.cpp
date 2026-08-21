// =============================================================================
// ForgeKV — Stage 6: Server entry point
// =============================================================================
//
// Starts a ForgeKV HTTP server on the configured host and port.
//
// Usage:
//   ./forgekv_server [port]
//
//   port — TCP port to listen on. Defaults to 8080 if not provided.
//           Pass 0 to let the OS assign an ephemeral port.
//
// Startup sequence:
//   1. Construct KeyValueStore (performs WAL recovery automatically).
//   2. Construct HttpServer wrapping the store.
//   3. Print the configured host/port to stdout.
//   4. Call server.listen() — blocks until the process is interrupted.
//
// Shutdown:
//   Send SIGINT (Ctrl-C) or SIGTERM to stop the server. The process exits
//   with status 0 on clean shutdown, 1 on error.
//
// WAL file:
//   The default KeyValueStore constructor opens "forgekv.wal" in the current
//   working directory. Run the server from the project root, or change
//   directory before launching.
//
// Single-threaded note:
//   Stage 6 processes all requests serially. Do not use this server under
//   concurrent load until Stage 7 (concurrency) is implemented.
// =============================================================================

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    // -------------------------------------------------------------------------
    // Parse optional port argument.
    // -------------------------------------------------------------------------
    int port = 8080;

    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

    if (argc == 2) {
        try {
            port = std::stoi(argv[1]);
            if (port < 0 || port > 65535) {
                std::cerr << "Error: port must be between 0 and 65535\n";
                return 1;
            }
        } catch (const std::exception&) {
            std::cerr << "Error: invalid port '" << argv[1] << "'\n";
            return 1;
        }
    }

    // -------------------------------------------------------------------------
    // Construct the KV engine.
    //
    // The default KeyValueStore constructor:
    //   - Creates InMemoryStorage.
    //   - Opens "forgekv.wal" in the current working directory (append mode).
    //   - Replays the WAL to reconstruct state from the last run.
    // -------------------------------------------------------------------------
    forgekv::KeyValueStore store;

    // -------------------------------------------------------------------------
    // Construct the HTTP server.
    //
    // The constructor:
    //   - Installs InlineTaskQueue for single-threaded operation.
    //   - Registers all four REST endpoints (with 501 stubs at Stage 6A).
    // -------------------------------------------------------------------------
    forgekv::HttpServer server(store);

    // -------------------------------------------------------------------------
    // Start listening. This call blocks until stop() or process exit.
    // -------------------------------------------------------------------------
    const std::string host = "127.0.0.1";

    std::cout << "ForgeKV HTTP server starting\n";
    std::cout << "  Host : " << host << "\n";
    std::cout << "  Port : " << port << "\n";
    std::cout << "  Mode : single-threaded (Stage 6)\n";
    std::cout << "  Press Ctrl-C to stop.\n\n";

    const bool ok = server.listen(host, port);

    if (!ok) {
        std::cerr << "Error: server failed to bind on "
                  << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "\nForgeKV server stopped.\n";
    return 0;
}
