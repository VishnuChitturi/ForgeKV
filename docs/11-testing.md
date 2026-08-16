# 11 — Testing

> **Stage:** 13  
> **Status:** 🔲 Planned. This document describes the testing strategy. No tests exist yet.

---

## Philosophy

The test suite exists to prove ForgeKV is reliable, not merely functional. A system that works under ideal conditions but breaks under partial failures, concurrent access, or unexpected input is not a storage engine — it is a demo.

Tests are introduced alongside each stage. By Stage 13, a complete test suite covers every component and interaction path.

---

## Test Categories

### 1. Unit Tests

Unit tests verify individual components in complete isolation. External dependencies are replaced with test doubles or not used at all.

| Component             | What is tested                                               |
|-----------------------|--------------------------------------------------------------|
| `KeyValueStore`       | SET, GET, DELETE, EXISTS semantics and edge cases            |
| WAL writer            | Correct record format, append behavior, fsync               |
| WAL reader/parser     | Parsing valid records, detecting partial/corrupt records     |
| Binary serialization  | Round-trip encoding/decoding of records                      |
| Checksum              | CRC32 correct for known inputs, detects single-bit flips     |
| Snapshot writer       | Correct file format, atomic write (temp + rename)            |
| Snapshot reader       | Load and reconstruct state from snapshot file                |
| TTL logic             | Expiry timestamp calculation, lazy expiration checks         |
| Statistics            | Correct counters after operations                            |

Unit tests are fast (milliseconds per test) and deterministic. They are the majority of the test suite.

### 2. Integration Tests

Integration tests verify that components work correctly together, using real dependencies rather than test doubles.

| Scenario                           | What is verified                                       |
|------------------------------------|--------------------------------------------------------|
| SET via HTTP → in-memory → WAL     | Full write path end-to-end                             |
| GET after SET                      | Value is readable after being stored                   |
| DELETE then GET                    | Key is absent after deletion                           |
| Restart with WAL                   | State is correctly recovered on restart                |
| Compaction → restart               | Compacted WAL produces correct state on recovery       |
| Snapshot → restart                 | Snapshot + WAL delta produces correct state            |
| TTL expiry → GET                   | Expired key returns not_found                          |
| Stats after operations             | Statistics reflect actual operation counts             |

Integration tests exercise real file I/O, real in-memory state, and real HTTP requests.

### 3. Persistence Tests

Persistence tests verify that data survives a process restart.

```
Procedure:
  1. Start ForgeKV process
  2. Write a set of known key-value pairs
  3. Shut down the process (clean or forced)
  4. Start a new ForgeKV process
  5. Verify every expected key is present with the correct value
  6. Verify deleted keys are absent
```

These tests directly validate the WAL write-then-replay guarantee.

### 4. WAL Format Tests

WAL format tests verify the binary record format independently of the rest of the system.

| Test                                  | What is verified                              |
|---------------------------------------|-----------------------------------------------|
| Write SET record, read it back        | Round-trip fidelity                           |
| Write DELETE record, read it back     | DELETE opcode and key encoding                |
| Write multiple records sequentially   | No record boundary corruption                 |
| Key at length boundary (empty, large) | Edge case key lengths handled correctly       |
| Value at length boundary              | Edge case value lengths handled correctly     |

### 5. Corruption and Recovery Tests

These tests verify that ForgeKV handles a damaged WAL gracefully.

| Scenario                              | Expected behavior                                   |
|---------------------------------------|-----------------------------------------------------|
| Truncated final record                | Discard partial record, recover all prior records   |
| Single-byte corruption in record body | Checksum failure detected, record discarded         |
| Corruption in the middle of WAL       | Recovery stops at first invalid record              |
| Empty WAL file                        | Store starts empty, no error                        |
| WAL file with only partial header     | Treated as partial record, discarded                |
| Valid WAL followed by garbage bytes   | Garbage detected, replay stops                      |

These tests simulate real-world failure modes: power cuts mid-write, filesystem corruption, disk errors.

### 6. Crash Recovery Tests

Crash recovery tests simulate process termination and verify recovery.

```
Approach:
  - Write operations via the engine API
  - Simulate crash: kill process at a specific point
    (or simply drop the in-memory state without flushing)
  - Restart and recover from WAL
  - Verify state matches the last known-good committed write
```

These tests validate the "durability guarantee" described in 04-crash-recovery.md: any operation whose WAL record was fully flushed before the crash must be present after recovery.

### 7. Concurrency Tests

Concurrency tests verify that simultaneous access from multiple threads is safe.

| Test                                 | What is verified                               |
|--------------------------------------|------------------------------------------------|
| Concurrent GET from N threads        | All reads return correct values, no crash      |
| Concurrent SET from N threads        | Final state is consistent (last write wins)    |
| Mixed GET + SET from N threads       | No data races, no lost updates                 |
| SET + DELETE racing on same key      | Final state reflects one of the valid outcomes |

Concurrency tests will run under **Thread Sanitizer (TSAN)**, which instruments memory accesses at compile time and reports data races at runtime. A test that passes under TSAN is strong evidence of correctness.

### 8. Stress Tests

Stress tests push ForgeKV under sustained load to surface failures that do not appear in short-running tests.

| Scenario                            | Duration / Scale                         |
|-------------------------------------|------------------------------------------|
| High-volume SET (millions of ops)   | Run until WAL triggers compaction        |
| Concurrent clients sustained load   | 10–100 threads for minutes               |
| Repeated restart with large WAL     | Measure recovery time at increasing sizes |
| TTL churn (create and expire keys)  | High volume of short-lived keys          |

Stress tests may be excluded from the standard test run (due to duration) but are run explicitly as a separate target.

---

## Test Framework

The test framework will be selected at Stage 13. Candidates:

- **GoogleTest (gtest)** — industry standard, good CMake integration, familiar to most C++ developers
- **Catch2** — header-only, simpler setup, good BDD-style test expressions

The decision will be made at implementation time.

---

## Test Coverage Goals

There is no specific line coverage target. Coverage metrics are a proxy for test quality, not a substitute for it. The goal is to cover:

- Every documented behavior
- Every documented edge case
- Every documented failure mode

A feature that is not tested is not considered done.

---

## Test Infrastructure in CMake

Tests will be registered with CMake's `CTest` system, enabling:

```bash
cmake --build build
ctest --test-dir build
```

Individual test binaries can also be run directly. A separate CMake build type (`-DCMAKE_BUILD_TYPE=Sanitize` or similar) will enable TSAN and ASAN (AddressSanitizer) instrumentation.

---

*Previous: [10-benchmarking.md](10-benchmarking.md)*  
*Next: [12-final-architecture.md](12-final-architecture.md)*
