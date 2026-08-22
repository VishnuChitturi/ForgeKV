# Stage 11 — Statistics / Observability

ForgeKV v0.11.0

---

## Overview

Stage 11 adds lightweight runtime statistics to ForgeKV and exposes them
through a new HTTP endpoint:

```
GET /stats
```

The statistics system is:

- **Thread-safe** — atomic counters updated without the main storage mutex;
  storage-derived metrics read under the shared lock.
- **Low overhead** — all counter increments use `std::memory_order_relaxed`
  atomic operations. No extra locking is introduced for observability.
- **Non-persisted** — all statistics are process-lifetime metrics only.
  No WAL records are written. No snapshot fields are added.
- **Non-resettable** — statistics reset naturally when a new `KeyValueStore`
  instance is created. There is no public reset API.

---

## Statistics Reference

### `key_count`

The number of currently live, non-expired keys in the store at the instant
`stats()` is called.

**Source:** derived from `Storage::size()` under the shared lock.

**Semantics:**
- A newly inserted key increments `key_count` immediately.
- An update to an existing key does **not** change `key_count`.
- A deletion decrements `key_count` immediately.
- An expiration decrements `key_count` as soon as the background cleanup
  thread removes it (via `run_cleanup_now()` or the next background pass).
- Expired keys that have not yet been physically removed may still occupy
  storage, but `Storage::size()` does **not** count them — the
  `InMemoryStorage` implementation filters expired entries in `size()`.

**Not a separate counter.** `key_count` is derived directly from the storage
layer. There is no risk of it going out of sync with the actual live key set.

---

### `get_hits`

The number of `get()` calls that returned a non-empty value (key found and
not expired).

**Incremented:** inside `KeyValueStore::get()` after `Storage::get()` returns
a value.

**Not incremented for:** recovery replays (recovery does not call `get()`),
`exists()` calls, or `ttl()` queries.

---

### `get_misses`

The number of `get()` calls that returned `nullopt` (key absent or expired).

**Incremented:** inside `KeyValueStore::get()` after `Storage::get()` returns
`nullopt`.

---

### `set_count`

The number of successful `set()` calls (permanent upsert).

**Incremented:** at the end of `KeyValueStore::set()` after the WAL write and
storage update succeed, but **only when not recovering**.

**Not incremented for:** `set_with_ttl()` calls, recovery replays, or
snapshot loading.

---

### `delete_count`

The number of explicit `del()` calls that found and deleted a key.

**Incremented:** inside `KeyValueStore::del()` after the WAL write and storage
deletion succeed, but **only when not recovering** and only when the key
existed at the time of the call.

**Semantics:**
- Deleting a key that does **not** exist (or is already expired) does **not**
  increment `delete_count`.
- Background expiration cleanup does **not** increment `delete_count`. Those
  removals are counted in `expired_count`.

---

### `ttl_set_count`

The number of `set_with_ttl()` calls where the key was actually stored
(i.e., `ttl_seconds > 0`).

**Incremented:** inside `KeyValueStore::set_with_ttl()` after the WAL write
and storage update succeed, but **only when not recovering** and only when
`ttl_seconds > 0`.

**Not incremented for:** calls where `ttl_seconds <= 0` (key immediately
expired, never stored), or recovery replays.

---

### `expired_count`

The number of keys physically removed by expiration processing.

**Incremented:** inside `do_expire_pass()` by the number of keys returned by
`Storage::expire_keys()`. This happens:
- Periodically on the background cleanup thread.
- Immediately on explicit `run_cleanup_now()` calls.

**Semantics:**
- Represents physical removals. A key that is logically expired (invisible
  through `get()`/`exists()`) but not yet physically removed is **not** yet
  counted in `expired_count`.
- Each key is counted exactly once, when it is removed.
- Does **not** overlap with `delete_count`.

---

### `wal_size_bytes`

The current size of the WAL file in bytes.

**Source:** `WAL::file_size()` — reads the actual on-disk file size via
`std::filesystem::file_size()`. Returns 0 if the WAL file does not exist.

**Semantics:**
- Increases after each write operation (`set`, `set_with_ttl`, `del`,
  `clear`).
- Decreases after `compact()` which rewrites the WAL with only the live state.
- Reflects the file size at the moment `stats()` acquires the shared lock.
- Does **not** call `fsync`. The reported size reflects the standard library's
  buffered view; unflushed OS buffers may contain additional data under
  extreme timing conditions, but this is consistent with the WAL's existing
  durability model.

---

### `uptime_seconds`

The elapsed time in seconds since the `KeyValueStore` instance was
constructed.

**Clock:** `std::chrono::steady_clock` (monotonic). Never goes backwards.

**Precision:** `double`, typically sub-millisecond resolution.

**Semantics:**
- Recorded at the very top of every constructor, **before** `recover()` runs.
  This means uptime includes WAL replay / snapshot loading time.
- Not persisted. Resets to 0 when a new `KeyValueStore` is created.
- Not a wall-clock timestamp. Do not use it to derive absolute dates/times.

---

### `last_snapshot_time_us`

The wall-clock time of the most recent **successful** snapshot, expressed as
microseconds since Unix epoch (UTC).

**Clock:** `std::chrono::system_clock` (wall clock), because the value must
correspond to a real-world time that can be compared across restarts.

**Zero** means no snapshot has ever succeeded in this process lifetime.

**Semantics:**
- Updated **only** on success. If `snapshot()` returns `false`, this field
  is **not** updated.
- Updated atomically inside `snapshot()`, under the exclusive lock, after the
  snapshot file has been written successfully.
- Not persisted. Resets to 0 when a new `KeyValueStore` is created.
- Not affected by `compact()`.

---

## TTL / Expiration Interaction

| Event | Effect on stats |
|-------|-----------------|
| `set_with_ttl(key, val, ttl)` where `ttl > 0` | `ttl_set_count++`, `key_count++` |
| `set_with_ttl(key, val, ttl)` where `ttl <= 0` | no change |
| Key expires (background cleanup) | `expired_count++`, `key_count--` |
| `get()` on expired key | `get_misses++` (storage returns nullopt) |
| `del()` on expired key | returns false; no counter incremented |

---

## Recovery Semantics

WAL replay and snapshot loading during `KeyValueStore` construction are
**not counted as client operations**.

The `recovering_` flag is set `true` before `recover()` is called and set
`false` after it returns. While `recovering_` is `true`:

- `set_count` is **not** incremented.
- `ttl_set_count` is **not** incremented.
- `delete_count` is **not** incremented.

The only exception is state-derived metrics: `key_count` is derived from
`Storage::size()` which correctly reflects the restored state after recovery.
`wal_size_bytes` reflects the WAL file on disk at the time of the call.

**Example:** After recovering from a WAL containing 3 SET + 1 DEL:

```
key_count    = 2   (correct: 2 live keys after recovery)
set_count    = 0   (no client sets yet)
delete_count = 0   (no client deletes yet)
```

---

## Uptime Semantics

Uptime is measured using `std::chrono::steady_clock`, which:

- Is monotonic (never decreases).
- Is not affected by system clock adjustments.
- Has no defined epoch — it measures elapsed duration, not absolute time.

The start point is captured in the constructor body before any other
initialization (`recover()` is called after `start_time_` is set). This
means uptime includes the time taken by WAL replay and snapshot loading.

---

## Snapshot Timestamp Semantics

`last_snapshot_time_us` uses Unix epoch microseconds (wall clock).

**Why microseconds?** Gives sub-millisecond precision with a compact
representation that fits in a `uint64_t`.

**Why wall clock?** The snapshot timestamp represents a real-world event time
that may need to be interpreted across process restarts. A monotonic clock
value would be meaningless after a restart.

**When zero?** No snapshot has ever succeeded in this process's lifetime. The
HTTP response will contain `"last_snapshot_time_us": 0`.

---

## Concurrency Model

| Operation | Lock held | Counter update |
|-----------|-----------|----------------|
| `set()` | exclusive (write) | `stat_set_count_` after release |
| `get()` | shared (read) | `stat_get_hits_` / `stat_get_misses_` after result known |
| `del()` | exclusive (write) | `stat_delete_count_` under write lock, before release |
| `set_with_ttl()` | exclusive (write) | `stat_ttl_set_count_` after storage update |
| `do_expire_pass()` | exclusive (write, cleanup thread) | `stat_expired_count_` after removal |
| `snapshot()` | exclusive (write) | `last_snapshot_time_us_` on success |
| `stats()` | shared (read, briefly) | reads atomics without lock |
| `compact()` | exclusive (write) | no counter changes |

All counters use `std::memory_order_relaxed`. This is safe because:

1. The counters are independent. No synchronisation of other memory is required
   through these counters.
2. `stats()` tolerates a slightly stale view. Observability data does not need
   to be a perfectly atomic snapshot.
3. The `[[nodiscard]]` pattern on `get()` ensures callers handle the return
   value explicitly.

`stats()` acquires the shared lock briefly to read `Storage::size()` and
`WAL::file_size()`. All atomic counter reads happen **after** the lock is
released. This means the `key_count` and the operation counters in a single
`Stats` value may represent slightly different instants — this is acceptable
for observability.

---

## HTTP `/stats` Endpoint

```
GET /stats
```

**Response: HTTP 200**

```json
{
  "key_count":             42,
  "get_hits":              1200,
  "get_misses":            87,
  "set_count":             350,
  "delete_count":          44,
  "ttl_set_count":         91,
  "expired_count":         38,
  "wal_size_bytes":        12345,
  "uptime_seconds":        153.42,
  "last_snapshot_time_us": 1750000000000000
}
```

**Field types:**

| Field | JSON type | Notes |
|-------|-----------|-------|
| `key_count` | integer | live keys |
| `get_hits` | integer | |
| `get_misses` | integer | |
| `set_count` | integer | |
| `delete_count` | integer | |
| `ttl_set_count` | integer | |
| `expired_count` | integer | |
| `wal_size_bytes` | integer | bytes |
| `uptime_seconds` | float | 2 decimal places |
| `last_snapshot_time_us` | integer | 0 = never |

**No external JSON library is used.** The response is hand-serialized using
`std::ostringstream` with `std::fixed` and `std::setprecision(2)` for the
double field, and integer stream output for `uint64_t` fields.

**Error handling:** If an unexpected exception occurs while assembling the
stats response, the endpoint returns:

```
HTTP 500
{"error":"internal server error"}
```

No internal implementation details (paths, exception messages) are exposed to
clients.

---

## Design: `Stats` Struct

```cpp
// include/forgekv/stats.h

struct Stats {
    uint64_t key_count{0};
    uint64_t get_hits{0};
    uint64_t get_misses{0};
    uint64_t set_count{0};
    uint64_t delete_count{0};
    uint64_t ttl_set_count{0};
    uint64_t expired_count{0};
    uint64_t wal_size_bytes{0};
    double   uptime_seconds{0.0};
    uint64_t last_snapshot_time_us{0};
};
```

`Stats` is a plain value type. It is returned by `KeyValueStore::stats()` by
value. HTTP code receives an ordinary struct copy; it never sees atomic types
or holds a lock while serializing.

---

## Design: Counter Placement

```cpp
// Inside KeyValueStore (private members):

mutable std::atomic<uint64_t> stat_get_hits_{0};
mutable std::atomic<uint64_t> stat_get_misses_{0};
        std::atomic<uint64_t> stat_set_count_{0};
        std::atomic<uint64_t> stat_delete_count_{0};
        std::atomic<uint64_t> stat_ttl_set_count_{0};
        std::atomic<uint64_t> stat_expired_count_{0};
        std::atomic<uint64_t> last_snapshot_time_us_{0};

std::chrono::steady_clock::time_point start_time_;
bool recovering_{false};
```

`stat_get_hits_` and `stat_get_misses_` are `mutable` because `get()` is
`const` but must update them.

`start_time_` is declared first among the members so it is initialized before
any other member in constructor initializer lists. The default member
initializer `{std::chrono::steady_clock::now()}` captures the construction
time automatically in every constructor.

---

## Limitations

1. **No per-key statistics.** The store tracks aggregate counts only.

2. **No rate metrics.** There is no requests-per-second or throughput
   calculation. Callers can compute rates by calling `/stats` at intervals and
   differencing the counters.

3. **Slightly stale view.** The `Stats` snapshot may reflect `key_count` and
   `wal_size_bytes` from a slightly different instant than the atomic counters.
   This is intentional and acceptable for observability.

4. **No histogram or percentile latency.** Latency measurement is out of
   scope for Stage 11. Benchmarking is Stage 12.

5. **No persisted statistics.** All counters reset on process restart. If
   you need persistent counters, export them to an external metrics system
   before shutdown.

6. **WAL size is buffered, not fsynced.** `WAL::file_size()` reads the
   on-disk file size. As of Stage 11, ForgeKV does not call `fsync`. Under
   extreme conditions (crash immediately after an `append_*` call), the file
   size reported may not include the most recent unflushed OS write. This is
   consistent with the WAL's durability model.

7. **`clear()` is not counted separately.** The `clear()` operation removes
   all keys but does not increment any counter except via the reduction in
   `key_count`.

---

## Version

This feature was added in **ForgeKV v0.11.0**.

---

## Suggested Next Stage

**Stage 12 — Benchmarking.** With Stage 11 providing operation counters and
uptime, Stage 12 can build on this foundation to measure throughput (ops/sec)
and latency percentiles under controlled load. The benchmark harness will use
the `stats()` API directly to sample counter deltas over a timed window.
