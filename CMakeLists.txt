cmake_minimum_required(VERSION 3.20)

project(
    ForgeKV
    VERSION 0.12.0
    DESCRIPTION "A persistent, concurrent key-value storage engine built from scratch in C++20"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# C++20 — required for std::optional, std::unordered_map::contains, etc.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Debug build if nothing is specified.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

message(STATUS "ForgeKV version  : ${PROJECT_VERSION}")
message(STATUS "Build type       : ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard     : C++${CMAKE_CXX_STANDARD}")
message(STATUS "Compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

# ---------------------------------------------------------------------------
# forgekv_core — static library
# ---------------------------------------------------------------------------
# All ForgeKV implementation files are compiled into this library.
# Both the test binary and the demo binary link against it.

add_library(forgekv_core STATIC
    src/kv_store.cpp
    src/in_memory_storage.cpp   # Stage 2: InMemoryStorage implementation
    src/wal.cpp                 # Stage 4: Binary WAL + CRC32 checksums
    src/recovery.cpp            # Stage 5: WAL replay / crash recovery
    src/snapshot.cpp            # Stage 9: Snapshot manager
)

# PUBLIC: anything that links forgekv_core also gets this include path.
target_include_directories(forgekv_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# Treat warnings as errors in development builds to catch issues early.
target_compile_options(forgekv_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
        -Wall -Wextra -Wpedantic -Werror>
)

# ---------------------------------------------------------------------------
# forgekv_tests — test binary (Stage 1 + Stage 2 + Stage 3 + Stage 4)
# ---------------------------------------------------------------------------

add_executable(forgekv_tests
    tests/test_kv_store.cpp
)

target_link_libraries(forgekv_tests PRIVATE forgekv_core)

# Stage 7: concurrency tests use std::thread / std::latch — link pthreads.
find_package(Threads REQUIRED)
target_link_libraries(forgekv_tests PRIVATE Threads::Threads)

# Enable CTest integration so `ctest` discovers and runs the binary.
enable_testing()
add_test(NAME forgekv_all_tests COMMAND forgekv_tests)

# ---------------------------------------------------------------------------
# forgekv_server — Stage 6: HTTP server binary
# ---------------------------------------------------------------------------
# src/http_server.cpp implements HttpServer using vendored cpp-httplib.
# src/main.cpp is the entry point.
#
# cpp-httplib is a single-header library; no separate library target is needed.
# We add the vendored include path only to this target so it does not pollute
# forgekv_core or the test binary.

add_executable(forgekv_server
    src/http_server.cpp
    src/main.cpp
)

target_link_libraries(forgekv_server PRIVATE forgekv_core)

# Expose the vendored third-party headers (httplib.h) to this target.
target_include_directories(forgekv_server PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# cpp-httplib v0.18.5 uses std::thread internally (even when InlineTaskQueue
# is installed, the header includes <thread>). Link pthreads on Linux/macOS.
find_package(Threads REQUIRED)
target_link_libraries(forgekv_server PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# forgekv_http_tests — Stage 6: HTTP integration tests
# ---------------------------------------------------------------------------
# Tests the HttpServer REST API end-to-end using cpp-httplib's Client.
# The test binary starts a server on an ephemeral port per test fixture,
# sends real HTTP requests via httplib::Client, and verifies responses.

add_executable(forgekv_http_tests
    tests/test_http_server.cpp
    src/http_server.cpp        # HttpServer implementation
)

target_link_libraries(forgekv_http_tests PRIVATE forgekv_core)

# The HTTP test binary needs:
#   - the vendored httplib.h (for both server and client)
#   - the ForgeKV include dir (transitively from forgekv_core, but also PRIVATE)
target_include_directories(forgekv_http_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# cpp-httplib uses std::thread; link pthreads on Linux/macOS.
target_link_libraries(forgekv_http_tests PRIVATE Threads::Threads)

# Register with CTest.
add_test(NAME forgekv_http_all_tests COMMAND forgekv_http_tests)

# ---------------------------------------------------------------------------
# forgekv_demo — Stage 1 example / demo binary
# ---------------------------------------------------------------------------

add_executable(forgekv_demo
    examples/demo.cpp
)

target_link_libraries(forgekv_demo PRIVATE forgekv_core)

# ---------------------------------------------------------------------------
# forgekv_demo1 — Stage 1 interactive example
# ---------------------------------------------------------------------------

add_executable(forgekv_demo1
    examples/demo1.cpp
)

target_link_libraries(forgekv_demo1 PRIVATE forgekv_core)

# ---------------------------------------------------------------------------
# forgekv_benchmark — Stage 12: Benchmark suite
# ---------------------------------------------------------------------------
#
# Standalone benchmark executable. NOT added to CTest — benchmarks are not
# unit tests and should not run automatically in CI or during normal testing.
#
# Run manually:
#   ./build/forgekv_benchmark
#   ./build/forgekv_benchmark --operations 50000 --threads 4
#   ./build/forgekv_benchmark --output results.csv
#   ./build/forgekv_benchmark --help
#
# The benchmark links forgekv_core (KV engine) and includes http_server.cpp
# directly (for the HTTP benchmark). It also needs the httplib include path.

add_executable(forgekv_benchmark
    benchmarks/benchmark_main.cpp
    benchmarks/benchmark_kv_store.cpp
    benchmarks/benchmark_http.cpp
    src/http_server.cpp            # HttpServer implementation (for HTTP bench)
)

target_link_libraries(forgekv_benchmark PRIVATE forgekv_core)

# Benchmark binary includes:
#   - benchmarks/ for bench_harness.h
#   - third_party/httplib for httplib.h (HTTP benchmark client + server)
target_include_directories(forgekv_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/benchmarks
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# cpp-httplib uses std::thread; std::barrier also needs pthreads.
find_package(Threads REQUIRED)
target_link_libraries(forgekv_benchmark PRIVATE Threads::Threads)

# Benchmarks are NOT registered with CTest.
# Do NOT add_test() for forgekv_benchmark.

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Targets configured:")
message(STATUS "  forgekv_core       — static library (kv_store.cpp, in_memory_storage.cpp, wal.cpp, recovery.cpp, snapshot.cpp)")
message(STATUS "  forgekv_tests      — test binary    (tests/test_kv_store.cpp)  [Stage 1–12]")
message(STATUS "  forgekv_server     — HTTP server    (src/http_server.cpp, src/main.cpp)  [Stage 6–12]")
message(STATUS "  forgekv_http_tests — HTTP tests     (tests/test_http_server.cpp)          [Stage 6–12]")
message(STATUS "  forgekv_benchmark  — benchmark      (benchmarks/)  [Stage 12]  — NOT in CTest")
message(STATUS "  forgekv_demo       — demo binary    (examples/demo.cpp)")
message(STATUS "  forgekv_demo1      — demo binary    (examples/demo1.cpp)")
message(STATUS "")
message(STATUS "Build:           cmake --build build")
message(STATUS "Run tests:       ctest --test-dir build --output-on-failure")
message(STATUS "Run server:      ./build/forgekv_server [port]")
message(STATUS "Run benchmark:   ./build/forgekv_benchmark [--help]")
message(STATUS "Run demo:        ./build/forgekv_demo")
message(STATUS "")
