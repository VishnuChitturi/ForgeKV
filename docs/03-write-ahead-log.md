# 03 — Write-Ahead Log (Text WAL)

> **Stage:** 3 — Text Write-Ahead Log  
> **Status:** ✅ Complete. Implemented and tested.  
> **Version:** v0.3.0

---

## Table of Contents

1. [The Problem: RAM is Volatile](#the-problem-ram-is-volatile)
2. [The Solution: Write-Ahead Log](#the-solution-write-ahead-log)
3. [Architecture](#architecture)
4. [Write Ordering](#write-ordering)
5. [WAL Record Format](#wal-record-format)
6. [Example WAL Contents](#example-wal-contents)
7. [Operations and WAL Behavior](#operations-and-wal-behavior)
8. [File Handling](#file-handling)
9. [Error Handling](#error-handling)
10. [Format Constraints (Stage 3 Limitations)](#format-constraints-stage-3-limitations)
11. [What Stage 3 Does NOT Implement](#what-stage-3-does-not-implement)
12. [New Files](#new-files)
13. [Modified Files](#modified-files)

---

## The Problem: RAM is Volatile

After Stage 2, ForgeKV's entire state lives in `InMemoryStorage`, which wraps a `std::unordered_map` in RAM. RAM is volatile: when the process exits, crashes, or the machine loses power, every key-value pair is gone.

```
Without persistence:

  store.set("account_balance", "10000");
  // process crash or SIGKILL
  // on restart: the store is empty — data lost
```

For a storage engine, this is the fundamental unsolved problem. Data must outlive the process that created it.

---

## The Solution: Write-Ahead Log

A **Write-Ahead Log** is an append-only file on disk. Every time the store is mutated — every SET, every DEL, every CLEAR — a record of that mutation is written to the WAL file **before** the in-memory state is updated.

"Write-ahead" means: the log entry is written first. The in-memory state is updated second. Never the reverse.

```
Mutation request arrives
        │
        ▼
  Write record to WAL file on disk
        │
        ▼  (only after WAL write succeeds)
  Update in-memory state (Storage)
        │
        ▼
  Return success to caller
```

If the WAL write fails for any reason, the in-memory state is **not** modified. The exception propagates to the caller, who can observe that the operation failed.

This invariant ensures the WAL and in-memory state are never out of sync due to a partial failure.

---

## Architecture

Stage 3 preserves the Stage 2 storage abstraction and adds a WAL alongside it:

```
                  KeyValueStore
                 /             \
                /               \
               v                 v
           Storage             WAL
               |                 |
               v                 v
       InMemoryStorage      forgekv.wal (text)
               |
               v
         unordered_map
```

The WAL is a separate component with its own responsibility: writing records to disk. It does not know about in-memory state, keys, or recovery. `KeyValueStore` coordinates both.

The WAL lives in:

- `include/forgekv/wal.h` — class declaration and documentation
- `src/wal.cpp` — implementation

`KeyValueStore` owns a `std::unique_ptr<WAL>` alongside `std::unique_ptr<Storage>`. Both are injected at construction time or created with sensible defaults.

---

## Write Ordering

The write-ordering invariant for each mutating operation:

### SET

```
KeyValueStore::set(key, value)
        │
        ▼
  wal_->append_set(key, value)   ← WAL write FIRST
        │
        ▼  (only if WAL write succeeds)
  storage_->set(key, value)      ← in-memory update SECOND
```

### DEL

```
KeyValueStore::del(key)
        │
        ├─── storage_->exists(key) returns false
        │         │
        │         └──→ return false immediately (no WAL write, no storage change)
        │
        └─── storage_->exists(key) returns true
                  │
                  ▼
          wal_->append_del(key)   ← WAL write FIRST
                  │
                  ▼
          storage_->del(key)      ← in-memory removal SECOND
                  │
                  ▼
          return true
```

### CLEAR

```
KeyValueStore::clear()
        │
        ▼
  wal_->append_clear()    ← WAL write FIRST
        │
        ▼
  storage_->clear()       ← in-memory wipe SECOND
```

---

## WAL Record Format

Each record occupies exactly one line. The format is:

| Operation | Record format          | Example                    |
|-----------|------------------------|----------------------------|
| SET       | `SET\|<key>\|<value>`  | `SET\|name\|Vishnu`        |
| DEL       | `DEL\|<key>`           | `DEL\|age`                 |
| CLEAR     | `CLEAR`                | `CLEAR`                    |

- The field delimiter is `|` (pipe character).
- Each record is terminated by a `\n` (Unix newline).
- Records are appended sequentially; the file is never rewritten.

### Why `|` as delimiter?

`|` is uncommon in typical keys and values, making it easy to parse. The format is human-readable and inspectable with any text editor or `cat`.

### Read operations are NOT logged

`get()`, `exists()`, `size()`, and `empty()` are pure reads. They do not modify state, so they produce no WAL records.

---

## Example WAL Contents

After this sequence of operations:

```cpp
store.set("name", "Vishnu");
store.set("age",  "21");
store.set("city", "Bengaluru");
store.del("age");
store.set("name", "Vishnu Kumar");
store.clear();
store.set("project", "ForgeKV");
store.set("stage",   "3");
```

The WAL file (`forgekv.wal`) contains:

```
SET|name|Vishnu
SET|age|21
SET|city|Bengaluru
DEL|age
SET|name|Vishnu Kumar
CLEAR
SET|project|ForgeKV
SET|stage|3
```

This file is plain text. You can inspect it at any time:

```bash
cat build/forgekv.wal
```

---

## Operations and WAL Behavior

| Operation  | WAL record written? | Condition                              |
|------------|--------------------|-----------------------------------------|
| `set(k,v)` | Yes — `SET\|k\|v`  | Always                                  |
| `del(k)`   | Yes — `DEL\|k`     | Only when key exists in storage         |
| `del(k)`   | No                 | When key does not exist (no-op)         |
| `clear()`  | Yes — `CLEAR`      | Always                                  |
| `get(k)`   | No                 | Read-only, no state change              |
| `exists(k)`| No                 | Read-only, no state change              |
| `size()`   | No                 | Read-only, no state change              |
| `empty()`  | No                 | Read-only, no state change              |

### Why only log DEL when the key exists?

A DEL on a non-existent key changes nothing. The WAL should be a log of state changes, not of attempted operations. Logging a no-op DEL would create spurious records that a future recovery stage would have to handle or ignore. Keeping only meaningful records makes the WAL semantically clean.

### Why CLEAR gets its own record?

`CLEAR` removes all keys at once. Individual DEL records would be impractical for large stores. A single `CLEAR` record is unambiguous and efficient.

---

## File Handling

- The WAL file is opened with `std::ofstream` in **append mode** (`std::ios::app`).
- If the file does not exist, it is created on first open.
- If the file already exists, new records are appended — existing content is never truncated or overwritten.
- Each record is flushed immediately after writing (`stream_.flush()`), so the OS buffer is drained after every operation.
- The file is closed automatically when the `WAL` object is destroyed (RAII via `std::ofstream`).

### Default WAL path

When `KeyValueStore` is constructed with its default constructor, the WAL is opened at:

```
forgekv.wal
```

(relative to the current working directory). When running tests via `ctest`, this resolves to the `build/` directory, which is gitignored — the source tree is never polluted.

### Configuring the WAL path

Pass a `std::unique_ptr<WAL>` to the full dependency-injection constructor:

```cpp
auto wal = std::make_unique<forgekv::WAL>("/data/myapp.wal");
auto store = forgekv::KeyValueStore(
    std::make_unique<forgekv::InMemoryStorage>(),
    std::move(wal)
);
```

---

## Error Handling

### WAL construction failure

If the log file cannot be opened (bad path, permissions, missing directory), the `WAL` constructor throws `std::runtime_error`:

```cpp
// Throws std::runtime_error — the caller must handle it
forgekv::WAL wal("/nonexistent/path/forgekv.wal");
```

This means constructing a `KeyValueStore` with a bad WAL path also throws. The store is never left in a usable state with a broken WAL.

### WAL write failure

If a write fails (disk full, file descriptor closed, I/O error), `append_set`, `append_del`, or `append_clear` throws `std::runtime_error`.

Because the WAL write happens **before** the in-memory mutation, a write failure leaves in-memory state unchanged. The store remains consistent: the operation failed atomically from the caller's perspective.

```cpp
// If this throws, storage is unchanged — the key was not set
store.set("key", "value");
```

### Error propagation

Errors are propagated as C++ exceptions (`std::runtime_error`). `std::cout` is not used for error reporting. Callers can catch and handle the exception, or let it propagate up the call stack.

---

## Format Constraints (Stage 3 Limitations)

Stage 3's text format has one significant constraint:

**Keys and values must not contain:**
- `|` — the field delimiter
- `\n` — the record terminator  
- `\r` — carriage return (would corrupt line parsing)

These constraints are **documented but not enforced at runtime** in Stage 3. If a key or value contains a `|`, the resulting WAL record will be malformed and will not parse correctly in a future recovery stage.

Stage 4 (Binary WAL) eliminates these constraints by using length-prefixed binary fields, which require no special escaping and support arbitrary byte sequences in keys and values.

For Stage 3, the practical implication is simple: use normal string keys and values. Keys like `"user:name"`, `"page:count"`, `"server-1"` are all fine. Keys containing `|` are not.

---

## What Stage 3 Does NOT Implement

Stage 3 provides **WAL writing only**. The following are explicitly out of scope:

| Feature                    | Stage |
|----------------------------|-------|
| Crash recovery / WAL replay | 5     |
| Binary WAL + checksums      | 4     |
| WAL compaction              | 8     |
| Thread-safe WAL writes      | 7     |
| Snapshots                   | 9     |
| HTTP server                 | 6     |
| TTL / key expiration        | 10    |
| Statistics                  | 11    |
| Benchmarking                | 12    |

**ForgeKV is NOT crash-recoverable after Stage 3.**

The WAL file is written to disk on every mutation, but on restart, the WAL is not read or replayed. The in-memory store starts empty. Stage 5 will add the recovery logic that reads the WAL on startup and reconstructs the in-memory state.

After Stage 5, the WAL file will enable crash recovery. After Stage 3, it is a log you can inspect but not yet replay automatically.

---

## New Files

| File                        | Description                                 |
|-----------------------------|---------------------------------------------|
| `include/forgekv/wal.h`     | WAL class declaration and documentation     |
| `src/wal.cpp`               | WAL implementation                          |

---

## Modified Files

| File                        | Change                                                    |
|-----------------------------|-----------------------------------------------------------|
| `include/forgekv/kv_store.h`| Added WAL dependency; added full DI constructor           |
| `src/kv_store.cpp`          | Implemented write-ahead ordering in set/del/clear         |
| `tests/test_kv_store.cpp`   | Added 18 Stage 3 WAL tests; updated S2-19 for new del() semantics |
| `CMakeLists.txt`            | Added `src/wal.cpp`; bumped version to 0.3.0              |

---

*Previous: [02-storage-abstraction.md](02-storage-abstraction.md)*  
*Next: [04-crash-recovery.md](04-crash-recovery.md)*
