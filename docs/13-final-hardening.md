# ForgeKV — Stage 13: Final Hardening

**Version:** 0.13.0  
**Status:** Complete  
**Test result:** 412 tests, 0 failures (12/12 CTest targets)

---

## Overview

Stage 13 is the final backend hardening stage for ForgeKV. No new user-facing features were added. The goal was to answer:

> "Can we trust ForgeKV after crashes, corruption, concurrency, snapshots, TTL expiration, and unusual inputs?"

The project includes coverage for the scenarios described below. It does not claim to be mathematically bug-free; these tests represent the best practical coverage achievable from the current design.

---

## Hardening Strategy

Each subsystem was tested against:
1. Normal usage (already covered in Stages 1–12)
2. Edge cases and boundary conditions (new in Stage 13)
3. Failure modes: corruption, truncation, unexpected state
4. Interaction effects: combined use of compaction + snapshot + TTL + restart

The test suite was extended without modifying existing tests or production code. Two genuine bugs were discovered and corrected in the tests themselves (not in production code — see below).

---

## Test Files Added

| File | Tests | Focus |
|------|-------|-------|
| `tests/test_wal_robustness.cpp` | 22 | WAL format, corruption, truncation, replay edge cases |
| `tests/test_recovery_hardening.cpp` | 15 | Realistic restart sequences |
| `tests/test_snapshot_hardening.cpp` | 20 | Snapshot robustness, corruption fallback |
| `tests/test_compaction_robustness.cpp` | 14 | Compaction state invariants, interactions |
| `tests/test_ttl_hardening.cpp` | 23 | TTL boundary cases, expiry, persistence |
| `tests/test_concurrency_hardening.cpp` | 12 | Thread-safety under concurrent workloads |
| `tests/test_http_edge_cases.cpp` | 18 | HTTP endpoint edge cases |
| `tests/test_boundary_cases.cpp` | 17 | Input boundaries: empty, long, binary, Unicode |
| `tests/test_lifecycle.cpp` | 13 | Resource lifecycle, cleanup thread, temp files |
| `tests/test_randomized.cpp` | 3 | Deterministic randomized state-machine tests |

**Total new tests: 157**  
**Total tests (all stages): 412**

---

## WAL Corruption Behavior

The WAL binary format (magic 0x464B5741, version 0x01, CRC32 checksum) handles corruption as follows:

| Scenario | Behavior |
|----------|----------|
| Empty WAL file | `replay()` returns 0 records, no error |
| Missing WAL file | `replay()` returns empty result (treated as new store) |
| Truncated final record only | Non-fatal: prior records applied, `incomplete_tail = true` |
| Corrupt CRC in any complete record | Fatal: `std::runtime_error` thrown |
| Invalid magic / version / opcode | Fatal: `std::runtime_error` thrown |
| Mid-log truncation (not final record) | Fatal: `std::runtime_error` thrown |
| Absurdly large key/val length field | Fatal: truncation error (no OOM) |

The key distinction: a truncated **final** record is a crash tail (tolerated). A corrupt **complete** record with a bad checksum is data corruption (fatal). This is documented in `include/forgekv/wal.h`.

### Covered by Stage 13 tests

- W1: Empty WAL replay (0 records, no error)
- W2–W6: Partial magic / header / key / value / CRC truncation → throws from `read_record`
- W7–W8: Invalid opcodes (0x00, 0xFF) → throws
- W9–W10: 1 GB key/val length fields → throws (truncation, not OOM)
- W11: Multiple valid + truncated tail → `incomplete_tail = true`, prior records applied
- W12: Multiple valid + corrupt final record CRC → throws (corruption ≠ truncation)
- W13: Corruption in middle record → throws (mid-log is fatal)
- W14–W15: `replay_from()` at EOF / record boundary
- W16: `kOpSetWithExpiry` binary record roundtrip
- W17: CRC field itself flipped → throws
- W18: `replay_from()` at mid-record offset → throws
- W19: Missing file returns empty result (documented design)
- W20–W22: DEL+CLEAR sequence, empty key/value, CLEAR-only WAL

---

## Recovery Behavior

Recovery (`KeyValueStore::recover()`) runs on every construction:

1. If a valid snapshot exists: load it, then replay WAL tail from `snap.wal_offset`.
2. If no snapshot or a corrupt snapshot: full WAL replay from offset 0.

A corrupt snapshot triggers a warning to stderr and falls back to full WAL replay — the corrupt snapshot does **not** destroy WAL recovery.

Recovery does **not** increment operation counters (`set_count`, `delete_count`, etc.). Stats counters are zero immediately after construction.

### Covered by Stage 13 tests

- RH1–RH4: SET → restart, SET+DEL → restart, updates → restart, 500 keys → restart
- RH5–RH7: TTL key before/after expiry, permanent + TTL together
- RH8–RH11: Snapshot interactions, compaction interactions
- RH12: 3x reopen cycles are deterministic
- RH13: CLEAR followed by SET → restart
- RH14: Crash-style truncated WAL tail
- RH15: Stats counters zero after recovery

---

## Snapshot Failure Behavior

| Scenario | Behavior |
|----------|----------|
| Corrupt magic | `load()` returns `corrupt=true`; recovery falls back to WAL |
| Invalid version byte | `corrupt=true` |
| CRC mismatch | `corrupt=true` |
| Truncated file | `corrupt=true` |
| Missing file | `exists=false`; full WAL recovery |

A bad snapshot never silently produces incorrect state. The WAL is always sufficient to recover the correct state independently.

`compact()` **deletes the snapshot** before rewriting the WAL. This prevents a stale snapshot (pointing into the old WAL's byte offsets) from being used after compaction.

### Covered by Stage 13 tests

- SH1–SH5: Empty snapshot, 200-key snapshot, permanent/TTL keys, expired keys excluded
- SH6–SH10: Corruption scenarios → WAL fallback
- SH11: Missing snapshot → WAL-only
- SH12: Snapshot replacement (second call overwrites first)
- SH13–SH16: WAL tail applied after snapshot; compact() removes snapshot
- SH17: WAL offset prevents pre-snapshot records from being re-replayed
- SH18–SH20: Truncated payload, TTL recovery, stats timestamp

---

## Compaction Safety

After `compact()`:
- State before == state after (verified for up to 500 keys with 5 update rounds)
- Expired keys are **excluded** from the compacted WAL
- TTL metadata for live keys is **preserved**
- The snapshot file is deleted (prevents stale snapshot references)
- The WAL is atomically replaced (rename-based, same semantics as snapshot writes)

### Covered by Stage 13 tests

CR1–CR14: repeated SET, SET→DEL, SET→DEL→SET, many-key state preservation, TTL exclusion, compact+restart, compact+snapshot, snapshot+compact, double compact, mixed keys.

---

## TTL Edge Cases

| Input | Behavior |
|-------|----------|
| `set_with_ttl(k, v, 0.0)` | Key NOT stored (ttl_set_count not incremented) |
| `set_with_ttl(k, v, -1.0)` | Key NOT stored |
| `del()` on expired key | Returns `false` (expired == not found) |
| `get()` on expired key | Returns `nullopt` |
| `exists()` on expired key | Returns `false` |
| `ttl()` on expired key | Returns `kTtlNotFound` |
| `set()` after `set_with_ttl()` | Removes TTL, key becomes permanent |
| Restart before expiry | Key survives recovery (WAL contains the expiry timestamp) |
| Restart after expiry | Key is skipped by recovery (`expires_at_us <= now_us` is filtered) |

### Covered by Stage 13 tests

TH1–TH23: all TTL edge cases including zero/negative TTL, boundary timing, persistence across restart and snapshot, compaction metadata preservation, and interaction with stats.

---

## Concurrency Testing

ForgeKV uses `std::shared_mutex`:
- Read operations (`get`, `exists`, `size`, `ttl`) acquire **shared** locks.
- Write operations (`set`, `del`, `clear`, `compact`, `snapshot`) acquire **exclusive** locks.
- Stats atomics (`stat_get_hits_` etc.) are updated without the storage lock.

### Covered by Stage 13 tests

CH1–CH12: concurrent GET, SET to disjoint keys, GET+SET (no corrupted values), GET+DELETE, SET+DELETE on same key, mixed ops, TTL ops, concurrent stats(), concurrent snapshot() with writers, concurrent compact() with readers, barrier-phased writes, TTL+snapshot+writers interaction.

Tests use `std::latch`, `std::barrier`, and `std::atomic`. No arbitrary sleeps for synchronization.

---

## HTTP Edge Cases

### Covered by Stage 13 tests

HEC1–HEC18: immediate PUT+GET roundtrip, stats fields present, health always OK, 64 KB value, repeated PUT, double DELETE, URL-encoded keys, JSON special chars, stats key_count/set_count/get_hits tracking, concurrent PUTs (6 threads × 20 keys), concurrent GETs on same key (8 threads × 30 ops), PUT empty body → 400, health under load, Content-Type always `application/json`, server restart persists data.

---

## Boundary and Input Tests

### Covered by Stage 13 tests

BC1–BC17: empty key, empty value, single char, 4096-byte key, 1 MB value, null bytes in key/value, JSON special chars, all control characters 0x00–0x1F, UTF-8 multibyte strings, whitespace-only key, long key+value WAL roundtrip, near-identical keys, empty value with TTL, pipe delimiters, 1000 distinct keys.

---

## Lifecycle and Resource Tests

### Covered by Stage 13 tests

LC1–LC13: construct+destroy empty store, reopen same WAL, independent stores, 10x create/destroy cycles, destructor joins cleanup thread quickly (< 3s), compact leaves no .tmp files, snapshot leaves no .tmp files, HTTP server start/stop, 3x server create/stop, WAL file exists after destruction, snapshot file absent before first snapshot(), run_cleanup_now safe with background thread, move-constructed store.

---

## Randomized State-Machine Tests

Three deterministic randomized tests use fixed seeds (printed on failure):

| Test | Seed | Ops | Description |
|------|------|-----|-------------|
| RZ1 | 0xFEEDC0DE | 5000 | SET/GET/DEL/EXISTS/TTL_SET against reference model |
| RZ2 | 0xABCD1234 | 500 | State machine with mid-sequence restart + recovery |
| RZ3 | 0xDEADBEEF | 400 | State machine with mid-sequence compaction |

The reference model (`ReferenceModel`) is a `std::unordered_map` with optional expiry timestamps. ForgeKV results are compared against the model. Timing races on TTL-expiring keys are tolerated by re-checking both sides after a potential race.

---

## Sanitizer Commands

Sanitizers are not part of normal CTest (platform compatibility varies). Run manually:

### AddressSanitizer + UndefinedBehaviorSanitizer

```bash
cmake -B build_asan -DCMAKE_BUILD_TYPE=Debug -DFORGEKV_ASAN=ON
cmake --build build_asan --parallel 4
cd build_asan
./forgekv_tests
./forgekv_tests_wal
./forgekv_tests_concurrency
./forgekv_tests_ttl
```

### ThreadSanitizer

```bash
cmake -B build_tsan -DCMAKE_BUILD_TYPE=Debug -DFORGEKV_TSAN=ON
cmake --build build_tsan --parallel 4
cd build_tsan
./forgekv_tests
./forgekv_tests_concurrency
./forgekv_tests_ttl
```

**Note:** ASAN and TSAN are mutually exclusive — do not combine them.

**Note on macOS:** ThreadSanitizer may report false positives for `std::condition_variable::wait_for` in some Apple Clang versions. These are known issues in the standard library instrumentation, not bugs in ForgeKV.

---

## Bugs Discovered and Fixed

Two test-side bugs were discovered during Stage 13 (no production code was modified):

### Bug 1: W19 — Wrong premise about WAL::replay() on missing file

**Original test:** Asserted that `WAL::replay()` throws when the underlying file is missing.  
**Actual behavior:** `WAL::replay()` returns an empty `ReplayResult{}` when the file does not exist. This is correct and documented behavior — a missing WAL file means an empty store, not an error.  
**Fix:** Updated W19 to test the correct semantics (returns empty result, no exception).

### Bug 2: LC6/LC7 — Empty `parent_path()` on macOS for relative paths

**Original test:** Used `std::filesystem::path(wal_path).parent_path()` to find the directory to scan. For a relative filename like `"test_lc_lc6.wal"`, `parent_path()` returns `""` (empty), which causes `directory_iterator("")` to throw on macOS.  
**Fix:** Fall back to `"."` when `parent_path()` is empty.

---

## Warning / Build Status

- Compilation: **0 warnings, 0 errors** after fixes
- All binaries compile with C++20, `-Wall -Wextra -Wpedantic -Werror` on `forgekv_core`
- Test binaries compile cleanly (warnings suppressed via `(void)` casts where `[[nodiscard]]` applies inside throw-testing macros)

---

## Final Test Counts

| Test binary | Tests | Status |
|-------------|-------|--------|
| `forgekv_tests` (Stage 1–12 baseline) | 218 | ✅ Pass |
| `forgekv_http_tests` (Stage 6–7 baseline) | 37 | ✅ Pass |
| `forgekv_tests_wal` | 22 | ✅ Pass |
| `forgekv_tests_recovery` | 15 | ✅ Pass |
| `forgekv_tests_snapshot` | 20 | ✅ Pass |
| `forgekv_tests_compaction` | 14 | ✅ Pass |
| `forgekv_tests_ttl` | 23 | ✅ Pass |
| `forgekv_tests_concurrency` | 12 | ✅ Pass |
| `forgekv_tests_http_edge` | 18 | ✅ Pass |
| `forgekv_tests_boundary` | 17 | ✅ Pass |
| `forgekv_tests_lifecycle` | 13 | ✅ Pass |
| `forgekv_tests_randomized` | 3 | ✅ Pass |
| **Total** | **412** | **0 failures** |

CTest result: **100% tests passed, 12/12 targets.**

---

## Known Limitations

1. **No fsync**: ForgeKV flushes standard library buffers (`stream.flush()`) but does not call `fsync()`. A power loss between the OS write-back and physical storage commit could lose the last operation. This is a known durability limitation documented since Stage 4.

2. **No crash isolation in tests**: Crash-style tests simulate partial WAL writes by file truncation rather than by actually crashing the process. A real process crash could leave OS-level buffers that are larger than the in-memory `flush()` state.

3. **Single snapshot file**: Only one snapshot is maintained at a time. A failed `snapshot()` that throws after the rename is theoretically possible but would leave the store in a valid state (either old snapshot or new snapshot, never half-written).

4. **ThreadSanitizer on macOS**: TSAN may report false positives in `std::condition_variable` code paths on some Apple Clang versions. These are instrumentation artifacts, not ForgeKV data races.

5. **Background cleanup thread granularity**: The default cleanup interval is 1 second. Expired keys may remain in memory for up to 1 second before removal. They are invisible to `get()`/`exists()` but do count toward peak memory usage.

6. **No distributed testing**: All tests run in a single process. Distributed failure modes (network partition, partial replication) are out of scope for this stage.

---

## Remaining Known Risks

- The WAL does not have a multi-writer lock at the file level. Two `KeyValueStore` instances pointing to the same WAL file would corrupt it. This is not a supported use case and is not tested.
- Large values (approaching available RAM) are not explicitly limited. The boundary tests cover 1 MB; behavior at swap pressure is untested.
- The randomized tests use a short fixed seed sequence. A much longer fuzz run might find additional state-transition edge cases not covered here.

---

*Stage 13 is the final backend version. ForgeKV v0.13.0.*
