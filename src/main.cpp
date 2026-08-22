// =============================================================================
// ForgeKV — Server entry point
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
// Port selection (in order of precedence):
//   1. Positional argument:  ./forgekv_server 9090
//   2. PORT environment variable:  PORT=9090 ./forgekv_server
//   3. Default: 8080
//
// Host:
//   The server binds to 0.0.0.0 (all interfaces) so that it is reachable
//   from outside the host, which is required when running in a container or
//   on a cloud platform. For local-only access, use a firewall or proxy.
//
// Startup sequence:
//   1. Construct KeyValueStore (performs WAL recovery automatically).
//   2. Construct HttpServer wrapping the store.
//   3. Print the resolved host/port to stdout.
//   4. Call server.listen() — blocks until the process is interrupted.
//
// Shutdown:
//   Send SIGINT (Ctrl-C) or SIGTERM to stop the server. The process exits
//   with status 0 on clean shutdown, 1 on error.
//
// WAL file:
//   The default KeyValueStore constructor opens "forgekv.wal" in the current
//   working directory. Run the server from the project root, or from a
//   dedicated data directory backed by persistent storage.
// =============================================================================

#include "forgekv/http_server.h"
#include "forgekv/kv_store.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char* argv[]) {
    // -------------------------------------------------------------------------
    // Resolve port.
    //
    // Precedence:
    //   1. Positional argument (argv[1])
    //   2. PORT environment variable
    //   3. Default: 8080
    // -------------------------------------------------------------------------
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [port]\n";
        return 1;
    }

    int port = 8080;
    std::string port_source = "default";

    if (argc == 2) {
        // Positional argument takes highest precedence.
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
        port_source = "argument";
    } else {
        // Check PORT environment variable.
        const char* port_env = std::getenv("PORT");
        if (port_env != nullptr && port_env[0] != '\0') {
            try {
                port = std::stoi(port_env);
                if (port < 0 || port > 65535) {
                    std::cerr << "Error: PORT environment variable must be between 0 and 65535\n";
                    return 1;
                }
            } catch (const std::exception&) {
                std::cerr << "Error: invalid PORT environment variable '" << port_env << "'\n";
                return 1;
            }
            port_source = "PORT env";
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
    // -------------------------------------------------------------------------
    forgekv::HttpServer server(store);

    // -------------------------------------------------------------------------
    // Start listening. This call blocks until stop() or process exit.
    //
    // Bind to 0.0.0.0 so the server is reachable on all network interfaces,
    // including inside containers and cloud-hosted VMs.
    // -------------------------------------------------------------------------
    const std::string host = "0.0.0.0";

    std::cout << "ForgeKV HTTP server starting\n";
    std::cout << "  Host   : " << host << "\n";
    std::cout << "  Port   : " << port << "  (source: " << port_source << ")\n";
    std::cout << "  WAL    : forgekv.wal (relative to working directory)\n";
    std::cout << "  Press Ctrl-C to stop.\n" << std::endl;

    const bool ok = server.listen(host, port);

    if (!ok) {
        std::cerr << "Error: server failed to bind on "
                  << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "\nForgeKV server stopped.\n";
    return 0;
}
