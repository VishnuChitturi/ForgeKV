cmake_minimum_required(VERSION 3.20)

project(
    ForgeKV
    VERSION 0.13.0
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
# Threads — required for std::thread, std::latch, std::barrier
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# forgekv_tests — test binary (Stage 1–12 baseline: 218 + 37 = 255 tests)
# ---------------------------------------------------------------------------

add_executable(forgekv_tests
    tests/test_kv_store.cpp
)

target_link_libraries(forgekv_tests PRIVATE forgekv_core Threads::Threads)

# ---------------------------------------------------------------------------
# Stage 13 hardening test binaries
# ---------------------------------------------------------------------------

# WAL robustness tests (22 tests)
add_executable(forgekv_tests_wal
    tests/test_wal_robustness.cpp
)
target_link_libraries(forgekv_tests_wal PRIVATE forgekv_core Threads::Threads)

# Recovery hardening tests (15 tests)
add_executable(forgekv_tests_recovery
    tests/test_recovery_hardening.cpp
)
target_link_libraries(forgekv_tests_recovery PRIVATE forgekv_core Threads::Threads)

# Snapshot hardening tests (20 tests)
add_executable(forgekv_tests_snapshot
    tests/test_snapshot_hardening.cpp
)
target_link_libraries(forgekv_tests_snapshot PRIVATE forgekv_core Threads::Threads)

# Compaction robustness tests (14 tests)
add_executable(forgekv_tests_compaction
    tests/test_compaction_robustness.cpp
)
target_link_libraries(forgekv_tests_compaction PRIVATE forgekv_core Threads::Threads)

# TTL hardening tests (23 tests)
add_executable(forgekv_tests_ttl
    tests/test_ttl_hardening.cpp
)
target_link_libraries(forgekv_tests_ttl PRIVATE forgekv_core Threads::Threads)

# Concurrency hardening tests (12 tests)
add_executable(forgekv_tests_concurrency
    tests/test_concurrency_hardening.cpp
)
target_link_libraries(forgekv_tests_concurrency PRIVATE forgekv_core Threads::Threads)

# HTTP edge cases tests (18 tests) — needs httplib
add_executable(forgekv_tests_http_edge
    tests/test_http_edge_cases.cpp
    src/http_server.cpp
)
target_link_libraries(forgekv_tests_http_edge PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_tests_http_edge PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# Boundary / input tests (17 tests)
add_executable(forgekv_tests_boundary
    tests/test_boundary_cases.cpp
)
target_link_libraries(forgekv_tests_boundary PRIVATE forgekv_core Threads::Threads)

# Lifecycle / resource tests (13 tests) — needs httplib
add_executable(forgekv_tests_lifecycle
    tests/test_lifecycle.cpp
    src/http_server.cpp
)
target_link_libraries(forgekv_tests_lifecycle PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_tests_lifecycle PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# Randomized state-machine tests (3 tests)
add_executable(forgekv_tests_randomized
    tests/test_randomized.cpp
)
target_link_libraries(forgekv_tests_randomized PRIVATE forgekv_core Threads::Threads)

# ---------------------------------------------------------------------------
# Enable CTest and register all test binaries
# ---------------------------------------------------------------------------
enable_testing()

# Baseline tests (Stage 1–12)
add_test(NAME forgekv_all_tests     COMMAND forgekv_tests)

# Stage 13 hardening tests
add_test(NAME forgekv_wal_tests         COMMAND forgekv_tests_wal)
add_test(NAME forgekv_recovery_tests    COMMAND forgekv_tests_recovery)
add_test(NAME forgekv_snapshot_tests    COMMAND forgekv_tests_snapshot)
add_test(NAME forgekv_compaction_tests  COMMAND forgekv_tests_compaction)
add_test(NAME forgekv_ttl_tests         COMMAND forgekv_tests_ttl)
add_test(NAME forgekv_concurrency_tests COMMAND forgekv_tests_concurrency)
add_test(NAME forgekv_http_edge_tests   COMMAND forgekv_tests_http_edge)
add_test(NAME forgekv_boundary_tests    COMMAND forgekv_tests_boundary)
add_test(NAME forgekv_lifecycle_tests   COMMAND forgekv_tests_lifecycle)
add_test(NAME forgekv_randomized_tests  COMMAND forgekv_tests_randomized)

# ---------------------------------------------------------------------------
# forgekv_http_tests — Stage 6/7 HTTP integration tests (37 tests)
# ---------------------------------------------------------------------------

add_executable(forgekv_http_tests
    tests/test_http_server.cpp
    src/http_server.cpp
)

target_link_libraries(forgekv_http_tests PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_http_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

add_test(NAME forgekv_http_all_tests COMMAND forgekv_http_tests)

# ---------------------------------------------------------------------------
# forgekv_tests_http_keys — Stage 16: GET /keys integration tests
# ---------------------------------------------------------------------------

add_executable(forgekv_tests_http_keys
    tests/test_http_keys.cpp
    src/http_server.cpp
)

target_link_libraries(forgekv_tests_http_keys PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_tests_http_keys PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

add_test(NAME forgekv_http_keys_tests COMMAND forgekv_tests_http_keys)

# ---------------------------------------------------------------------------
# forgekv_server — Stage 6: HTTP server binary
# ---------------------------------------------------------------------------

add_executable(forgekv_server
    src/http_server.cpp
    src/main.cpp
)

target_link_libraries(forgekv_server PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_server PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# ---------------------------------------------------------------------------
# forgekv_demo / forgekv_demo1 — example binaries
# ---------------------------------------------------------------------------

add_executable(forgekv_demo
    examples/demo.cpp
)
target_link_libraries(forgekv_demo PRIVATE forgekv_core)

add_executable(forgekv_demo1
    examples/demo1.cpp
)
target_link_libraries(forgekv_demo1 PRIVATE forgekv_core)

# ---------------------------------------------------------------------------
# forgekv_benchmark — Stage 12: Benchmark suite (NOT in CTest)
# ---------------------------------------------------------------------------

add_executable(forgekv_benchmark
    benchmarks/benchmark_main.cpp
    benchmarks/benchmark_kv_store.cpp
    benchmarks/benchmark_http.cpp
    src/http_server.cpp
)

target_link_libraries(forgekv_benchmark PRIVATE forgekv_core Threads::Threads)
target_include_directories(forgekv_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/benchmarks
    ${CMAKE_SOURCE_DIR}/third_party/httplib
)

# ---------------------------------------------------------------------------
# Stage 13: Sanitizer build targets (not part of normal CTest)
# ---------------------------------------------------------------------------
#
# These targets build the full test suite with sanitizers enabled.
# They are NOT registered with CTest to avoid platform-specific failures
# (e.g., TSAN and ASAN cannot run simultaneously, LeakSanitizer may require
# Linux, etc.). Run them manually as documented in docs/13-final-hardening.md.
#
# AddressSanitizer + UndefinedBehaviorSanitizer:
#   cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
#   cmake --build build_asan
#   ./build_asan/forgekv_tests_asan
#
# ThreadSanitizer:
#   cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug \
#         -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
#         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
#   cmake --build build_tsan
#   ./build_tsan/forgekv_tests_tsan
#
# Note: ASAN and TSAN are mutually exclusive. Do not combine them.

option(FORGEKV_ASAN  "Build with AddressSanitizer + UBSan" OFF)
option(FORGEKV_TSAN  "Build with ThreadSanitizer"          OFF)

if(FORGEKV_ASAN)
    message(STATUS "Building with AddressSanitizer + UBSan")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

if(FORGEKV_TSAN)
    message(STATUS "Building with ThreadSanitizer")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Targets configured:")
message(STATUS "  forgekv_core             — static library")
message(STATUS "  forgekv_tests            — baseline KV tests (Stage 1-12, 218 tests)")
message(STATUS "  forgekv_http_tests       — HTTP integration tests (37 tests)")
message(STATUS "  forgekv_tests_http_keys  — GET /keys integration tests (Stage 16, 21 tests)")
message(STATUS "  forgekv_tests_wal        — WAL robustness tests (22 tests)")
message(STATUS "  forgekv_tests_recovery   — Recovery hardening tests (15 tests)")
message(STATUS "  forgekv_tests_snapshot   — Snapshot hardening tests (20 tests)")
message(STATUS "  forgekv_tests_compaction — Compaction robustness tests (14 tests)")
message(STATUS "  forgekv_tests_ttl        — TTL hardening tests (23 tests)")
message(STATUS "  forgekv_tests_concurrency— Concurrency hardening tests (12 tests)")
message(STATUS "  forgekv_tests_http_edge  — HTTP edge cases tests (18 tests)")
message(STATUS "  forgekv_tests_boundary   — Boundary / input tests (17 tests)")
message(STATUS "  forgekv_tests_lifecycle  — Lifecycle / resource tests (13 tests)")
message(STATUS "  forgekv_tests_randomized — Randomized state-machine tests (3 tests)")
message(STATUS "  forgekv_server           — HTTP server binary")
message(STATUS "  forgekv_benchmark        — Benchmark suite [NOT in CTest]")
message(STATUS "  forgekv_demo / demo1     — Example binaries")
message(STATUS "")
message(STATUS "Build:           cmake --build build")
message(STATUS "Run tests:       ctest --test-dir build --output-on-failure")
message(STATUS "Run server:      ./build/forgekv_server [port]")
message(STATUS "Run benchmark:   ./build/forgekv_benchmark [--help]")
message(STATUS "ASan build:      cmake -B build_asan -DFORGEKV_ASAN=ON && cmake --build build_asan")
message(STATUS "TSan build:      cmake -B build_tsan -DFORGEKV_TSAN=ON && cmake --build build_tsan")
message(STATUS "")
