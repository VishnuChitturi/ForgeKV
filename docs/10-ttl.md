# ForgeKV — Stage 10: TTL / Expiration

## Overview

Stage 10 adds optional key expiration (TTL — Time To Live) to ForgeKV. A key
can be **persistent** (no expiration, the default) or **expiring** (it becomes
inaccessible after a configurable duration).

Expiration is **durable**: the expiry metadata is persisted in the WAL and in
snapshots. A restart does not resurrect an expired key.

---

## TTL Semantics

### Persistent vs Expiring Keys

| Key type   | Behaviour                                      |
|------------|------------------------------------------------|
| Persistent | Lives until explicitly deleted or `clear()`ed. |
| Expiring   | Lives until the absolute expiration time; then treated as absent. |

### Choosing the expiration representation

TTL is specified as a **relative duration** (seconds) when setting a key, but
is stored internally as an **absolute wall-clock timestamp** (microseconds since
Unix epoch, UTC, using `std::chrono::system_clock`).

Using an absolute timestamp avoids the ambiguity that arises with relative
values across process restarts: a key set with `ttl=10s` at time T should
expire at `T + 10s` regardless of when the process restarts.

### Update semantics

| Operation                        | Result                                          |
|----------------------------------|-------------------------------------------------|
| `set(key, value)`                | Upsert as permanent; **clears any existing TTL**. |
| `set_with_ttl(key, value, ttl)`  | Upsert with expiration at `now + ttl`.          |
| `set_with_ttl(key, value, 0.0)`  | Key is **not stored** (immediately expired).    |
| `set_with_ttl(key, value, < 0)`  | Key is **not stored** (immediately expired).    |

If a key already has a TTL and is overwritten with `set()`, the TTL is
removed — the key becomes permanent. If overwritten with `set_with_ttl()`, the
new expiration replaces the old one.

---

## API

### KeyValueStore

```cpp
// Set a key with an optional time-to-live in seconds.
// ttl_seconds > 0: key expires at now + ttl_seconds.
// ttl_seconds <= 0: key is not stored.
void set_with_ttl(const std::string& key,
                  const std::string& value,
                  double             ttl_seconds);

// Query the remaining TTL for a key (in seconds).
// Returns:
//   kTtlPermanent (-1.0): key exists, no expiration set.
//   kTtlNotFound  (-2.0): key does not exist or has already expired.
//   >= 0.0: seconds remaining until expiration.
double ttl(const std::string& key) const;

// Force an immediate expiration scan (primarily for testing).
// Under normal operation the background thread handles this.
void run_cleanup_now();
```

### Return value constants

```cpp
// In forgekv/kv_store.h:
inline constexpr double kTtlPermanent = -1.0; // key exists, no TTL
inline constexpr double kTtlNotFound  = -2.0; // key absent or expired
```

### Existing API (unchanged)

```cpp
void set(const std::string& key, const std::string& value); // permanent upsert
std::optional<std::string> get(const std::string& key);     // nullopt if expired
bool del(const std::string& key);                           // false if expired
bool exists(const std::string& key);                        // false if expired
std::size_t size();                                         // counts live keys only
bool empty();                                               // true if no live keys
void clear();
```

### HTTP API

The existing `PUT /key/:key` endpoint accepts an optional TTL header:

```
PUT /key/session
X-TTL-Seconds: 60
Content-Type: text/plain

<body is the value>
```

| Condition                             | Response |
|---------------------------------------|----------|
| Header absent                         | `200 OK` — key stored permanently. |
| Header present, value > 0             | `200 OK` — key stored with TTL. |
| Header present, value = 0             | `400 Bad Request` |
| Header present, value < 0             | `400 Bad Request` |
| Header present, value non-numeric     | `400 Bad Request` |

All other endpoints (`GET`, `DELETE`, `GET /health`) are unchanged.

---

## In-Memory Representation

Every key now maps to a `StoreEntry` struct rather than a plain string:

```cpp
struct StoreEntry {
    std::string   value;
    std::uint64_t expires_at_us{0}; // 0 = permanent; >0 = micros since epoch
};
```

The backing store in `InMemoryStorage` is:
```cpp
std::unordered_map<std::string, StoreEntry> store_;
```

`expires_at_us == 0` denotes a permanent key. `expires_at_us > 0` denotes an
expiring key with the absolute expiration time in microseconds since Unix epoch.

### Read-time expiration

`get()` and `exists()` check the current wall-clock time on every call. If
`now >= expires_at_us`, they return `nullopt` / `false` respectively — the key
is treated as absent even if it has not yet been physically removed from the
map.

This read-time check does NOT mutate storage. Physical removal happens only
under the exclusive lock (background cleanup or `run_cleanup_now()`).

---

## WAL Format

### Backward Compatibility

The existing WAL opcodes are untouched:

| Opcode | Value | Meaning                      |
|--------|-------|------------------------------|
| `kOpSet`   | `0x01` | Permanent SET (key/value).   |
| `kOpDel`   | `0x02` | DEL key.                     |
| `kOpClear` | `0x03` | CLEAR all keys.              |

Stage 10 adds one new opcode:

| Opcode | Value | Meaning                      |
|--------|-------|------------------------------|
| `kOpSetWithExpiry` | `0x04` | SET key/value with absolute expiry. |

Old WAL files (Stages 1–9) contain only opcodes `0x01`–`0x03` and are fully
recoverable without any changes to the recovery logic.

### SET_WITH_EXPIRY Record Layout

```
Offset  Size  Type       Field
------  ----  ---------  -------------------------------------------
     0     4  uint32_t   Magic number (0x464B5741, little-endian)
     4     1  uint8_t    Format version (0x01)
     5     1  uint8_t    Opcode (0x04 = SET_WITH_EXPIRY)
     6     4  uint32_t   Key length in bytes (little-endian)
    10     4  uint32_t   Value length in bytes (little-endian)
    14     K  bytes      Key bytes
  14+K     V  bytes      Value bytes
14+K+V     8  uint64_t   expires_at_us: absolute expiry, microseconds
                         since Unix epoch (UTC, little-endian)
14+K+V+8   4  uint32_t  CRC32 checksum (covers ALL preceding bytes)
```

The CRC32 checksum covers the header (14 bytes) + key + value +
`expires_at_us` (8 bytes). This ensures integrity of the timestamp itself.

---

## Recovery

Recovery handles all four opcodes:

| Opcode               | Recovery action                                        |
|----------------------|--------------------------------------------------------|
| `kOpSet`             | `storage.set(key, value)` — permanent key.             |
| `kOpSetWithExpiry`   | If `expires_at_us > now`: `storage.set_with_expiry(...)`. If already expired: **skip** (key is not restored). |
| `kOpDel`             | `storage.del(key)`.                                    |
| `kOpClear`           | `storage.clear()`.                                     |

**Expired entries are never resurrected.** A key whose `expires_at_us` has
already passed at recovery time is silently dropped — the final state matches
what would exist if the process had been running continuously.

Recovery uses snapshot-aware startup (Stage 9 behaviour preserved):

1. If a valid snapshot exists: load it, then replay the WAL tail from the
   snapshot's WAL offset.
2. If no snapshot or corrupt snapshot: full WAL replay from offset 0.

Both paths apply the same expiry filtering.

---

## Background Expiration Cleanup

### Design

A background `std::thread` (`cleanup_thread_`) runs inside `KeyValueStore` and
periodically removes expired keys. It wakes up:

- Every `cleanup_interval_` milliseconds (default: 1 second).
- Immediately when `stop_cleanup_` is set (shutdown signal).

Wakeup uses `std::condition_variable::wait_for()` with a predicate on
`stop_cleanup_`, so there is no busy-spinning or fixed arbitrary sleep.

### Cleanup pass

Under the exclusive lock (`std::unique_lock<std::shared_mutex>`):

1. Call `storage_->expire_keys(now_us)` — removes expired entries from the
   map and returns the list of removed keys.
2. For each removed key, call `wal_->append_del(key)` — writes a WAL DEL
   record.

Step 2 is critical: without it, a restart would replay the original
`SET_WITH_EXPIRY` record and resurrect the expired key. Writing a WAL DEL
makes the expiration durable.

### Durability guarantee

```
key expires (background thread)
    ↓
WAL DEL written (under exclusive lock)
    ↓
key removed from storage
    ↓
restart → replay WAL → DEL record removes key → key not restored
```

### Shutdown

`KeyValueStore::~KeyValueStore()`:

1. Sets `stop_cleanup_.store(true)`.
2. Calls `cleanup_cv_.notify_all()` — wakes the thread immediately.
3. Calls `cleanup_thread_.join()` — waits for the thread to exit.
4. Destroys `wal_` and `storage_` — only after the thread has exited,
   so there is no use-after-free.

The destructor never hangs: the thread checks `stop_cleanup_` after each
`wait_for()` and exits cleanly.

---

## Snapshot Interaction

### Format version

| Version | Value  | Records format                      |
|---------|--------|-------------------------------------|
| v1      | `0x01` | Stage 9 — no expiry fields.         |
| v2      | `0x02` | Stage 10 — `has_expiry` + optional `expires_at_us` per record. |

Stage 10 always **writes v2** snapshots. Stage 10 **reads both v1 and v2**:

- v1 snapshot → all entries are loaded as permanent (no expiry).
- v2 snapshot → entries with `has_expiry == 1` restore their expiry timestamp.

### v2 per-record layout

```
[key_len (4)] [key] [val_len (4)] [val]
[has_expiry (1)]
[expires_at_us (8)]  ← only present when has_expiry == 0x01
```

### Expired entries in snapshots

`snapshot()` calls `storage_->get_all_with_expiry(now_us)` which returns only
live (non-expired) entries. Expired keys are **never written** to the snapshot
file.

During snapshot load, `SnapshotManager::load()` also applies an additional
check: if a v2 record's `expires_at_us <= now` at load time, it is dropped
from the result and will not be restored to live storage.

---

## Compaction Interaction

`compact()` calls `storage_->get_all_with_expiry(now_us)`:

- **Expired keys are excluded** from the compacted WAL.
- **Live expiring keys** produce `kOpSetWithExpiry` records preserving their
  absolute expiration timestamp.
- **Permanent keys** produce `kOpSet` records (unchanged from Stage 8).

After compaction, the compacted WAL is a complete, minimal representation of
the current live state.

### Snapshot invalidation

As in Stage 9, `compact()` deletes any existing snapshot file before rewriting
the WAL. After compaction, recovery uses full WAL replay from offset 0 of the
new compacted WAL.

---

## Concurrency

The concurrency model from Stage 7 is unchanged:

| Operation              | Lock type        |
|------------------------|------------------|
| `get`, `exists`, `size`, `empty`, `ttl` | `shared_lock` |
| `set`, `set_with_ttl`, `del`, `clear`, `compact`, `snapshot` | `unique_lock` |
| background cleanup pass | `unique_lock` |

The background cleanup thread acquires the exclusive lock for its entire pass.
The pass is short (scan + WAL appends for expired keys), so write operations
are not blocked for a long time.

Read operations do NOT remove expired keys from storage (that would be a
mutation on a shared-lock path). They only check expiry and return "absent" for
expired keys. Physical removal is always done under an exclusive lock.

### No deadlocks

- The cleanup thread uses `cleanup_cv_mutex_` (a plain `std::mutex`) for the
  condition variable wait.
- It acquires `mutex_` (the `std::shared_mutex`) only during the cleanup pass,
  never while holding `cleanup_cv_mutex_`.
- Public methods acquire `mutex_` without touching `cleanup_cv_mutex_`.
- The destructor holds neither lock when it calls `notify_all()` and `join()`.

---

## Edge Cases

| Case                              | Behaviour                                      |
|-----------------------------------|------------------------------------------------|
| `set_with_ttl(k, v, 0.0)`        | Key not stored (immediately expired).          |
| `set_with_ttl(k, v, -5.0)`       | Key not stored.                                |
| `set_with_ttl(k, v, 60.0)` then `set(k, v2)` | Key becomes permanent, TTL removed. |
| `set_with_ttl(k, v1, 10s)` then `set_with_ttl(k, v2, 30s)` | Expiry replaced with new absolute timestamp. |
| `del()` on expired key            | Returns `false` (logically absent).            |
| `del()` on live expiring key      | Returns `true`; key removed; WAL DEL written. |
| `clear()` with expiring keys      | All keys removed including expiring ones.      |
| Restart before TTL expires        | Key restored with original expiry timestamp.   |
| Restart after TTL expires         | Key NOT restored (skipped during recovery).    |
| `compact()` before expiry         | Compacted WAL contains `SET_WITH_EXPIRY`.      |
| `compact()` after expiry          | Expired key excluded from compacted WAL.       |
| `snapshot()` before expiry        | Snapshot contains expiry metadata (v2).        |
| `snapshot()` after expiry         | Expired key excluded from snapshot.            |
| X-TTL-Seconds header missing      | HTTP PUT stores key permanently.               |
| X-TTL-Seconds: 0 or negative      | HTTP PUT returns 400 Bad Request.              |
| X-TTL-Seconds: non-numeric        | HTTP PUT returns 400 Bad Request.              |

---

## Durability / fsync Limitation

Stage 10 inherits the same durability model as Stages 3–9: writes are flushed
to the standard library buffer (`stream.flush()`) but not fsynced to disk.

In practice, on a power failure between a WAL write and the next OS-level
write-back, the WAL entry (including the expiry-deletion record written by the
cleanup thread) could be lost. This is consistent with the existing durability
guarantee across all stages.

---

## Files Added / Modified

### New file
- `docs/10-ttl.md` — this document.

### Modified files

| File | Change |
|------|--------|
| `include/forgekv/storage.h` | Added `StoreEntry` struct; added `set_with_expiry`, `get_entry`, `get_all_with_expiry`, `expire_keys` virtual methods. |
| `include/forgekv/in_memory_storage.h` | Added TTL method declarations; backing store changed to `unordered_map<string, StoreEntry>`. |
| `src/in_memory_storage.cpp` | Full rewrite with TTL support; all read ops check expiry at call time; `expire_keys()` physically removes expired entries. |
| `include/forgekv/wal.h` | Added `kOpSetWithExpiry = 0x04`, `kWalExpirySize = 8`, `expires_at_us` field in `WalRecord`, `append_set_with_expiry()`, `encode_u64()`, `SnapshotEntry` struct, updated `rewrite()` signature. |
| `src/wal.cpp` | Added `append_set_with_expiry`, `encode_u64`, updated `write_record` for the new opcode, updated `read_record` to handle opcode 0x04, updated `rewrite()` to accept `SnapshotEntry` and write `SET_WITH_EXPIRY` records. |
| `include/forgekv/snapshot.h` | Added `kSnapshotVersionV2 = 0x02`; `SnapshotData::records` changed from `vector<pair<string,string>>` to `vector<pair<string,StoreEntry>>`; `save()` signature updated. |
| `src/snapshot.cpp` | Full rewrite; `save()` writes v2 format with `has_expiry` flag per record; `load()` handles both v1 and v2 with expired-entry filtering. |
| `include/forgekv/kv_store.h` | Added `set_with_ttl`, `ttl`, `run_cleanup_now`, background thread members (`stop_cleanup_`, `cleanup_cv_`, `cleanup_thread_`), `kTtlPermanent`, `kTtlNotFound` constants, `~KeyValueStore()` destructor. |
| `src/kv_store.cpp` | Full rewrite; background cleanup thread with `condition_variable`; all four opcodes handled in `recover()`; `compact()` and `snapshot()` use expiry-aware storage queries; destructor joins thread. |
| `src/recovery.cpp` | Updated `run()` to handle `kOpSetWithExpiry` and skip already-expired entries. |
| `src/http_server.cpp` | PUT handler reads optional `X-TTL-Seconds` header; calls `set_with_ttl()` if present and valid, `set()` otherwise; validates header value. |
| `tests/test_kv_store.cpp` | 35 Stage 10 tests added (S10-A through S10-AI); `FakeStorage` and `TrackingStorage` updated with new virtual methods; `s9_snapshot_binary_format` updated to check `kSnapshotVersionV2`. |
| `tests/test_http_server.cpp` | 8 Stage 10 HTTP tests added (H10-A through H10-H). |
| `CMakeLists.txt` | Version bumped from `0.9.0` to `0.10.0`. |

---

## Test Summary

| Test suite              | Stage 1–9 tests | Stage 10 tests | Total |
|-------------------------|-----------------|----------------|-------|
| `forgekv_tests`         | 157             | 35             | 192   |
| `forgekv_http_tests`    | 17              | 8              | 25    |
| **Grand total**         |                 |                | **217** |

All 217 tests pass.

---

## Suggested Next Stage

**Stage 11 — Statistics / Observability**

Add operational metrics exposed via a REST endpoint or a `stats()` API:
- Key count (live keys only, respecting TTL).
- Operation counters: GET hits/misses, SET count, DEL count, TTL-set count.
- TTL expirations triggered by background cleanup vs. read-time expiry.
- WAL file size.
- Uptime.
- Snapshot age / last snapshot time.
