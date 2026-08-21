# Stage 9 — Snapshots

## What is a Snapshot?

A snapshot is a complete, point-in-time binary checkpoint of the current
key-value state. It records every live key-value pair together with a
WAL byte offset that marks exactly how far into the WAL the snapshot
represents.

Without snapshots, crash recovery requires replaying the entire WAL from
the beginning — regardless of how long the log has grown. With snapshots,
recovery can instead:

1. Load the snapshot (restore the stored key-value pairs directly).
2. Replay only the WAL records written **after** the snapshot.

This reduces both recovery time and the required WAL traversal to just the
"tail" since the last checkpoint.

---

## Conceptual View

### Before Snapshots (full WAL replay)

```
WAL from beginning
|
+-- SET A = 1          ← obsolete
+-- SET B = 2          ← live at this point
+-- SET A = 3          ← obsolete
+-- DEL B              ← obsolete
+-- SET C = 4          ← live
+-- SET A = 5          ← live final value
```

Recovery replays all 6 records every time.

### With Snapshots

```
Snapshot (captured after "SET A=3, DEL B")
|
+-- A = 3
(B was deleted — not stored)

WAL after snapshot boundary
|
+-- SET C = 4
+-- SET A = 5

Recovery:
  Snapshot  →  A=3
            ↓
  WAL tail  →  C=4, A=5 (A updated)
            ↓
  Final state: A=5, C=4
```

---

## Snapshot Binary Format

The snapshot uses a purpose-built binary format that is entirely distinct
from the WAL record format.

```
 Byte offset  Size  Type       Field
 -----------  ----  ---------  -------------------------------------------
           0     4  uint32_t   Magic  (0x464B534E — "FKSN" stored LE)
           4     1  uint8_t    Format version (0x01)
           5     8  uint64_t   WAL byte offset (little-endian)
          13     4  uint32_t   Record count (little-endian)
          17     …  records    Repeated for each key-value pair:
                                 key_len  (4 bytes, uint32_t, LE)
                                 key      (key_len bytes)
                                 val_len  (4 bytes, uint32_t, LE)
                                 val      (val_len bytes)
           ?     4  uint32_t   CRC32 checksum (little-endian)
                               Covers ALL bytes from offset 0 through the
                               last payload byte.
```

**Total header size:** 17 bytes (magic 4 + version 1 + wal_offset 8 + count 4).

**Magic bytes on disk:** `0x4E, 0x53, 0x4B, 0x46` ("NSKF" in ASCII).

All multi-byte integers are explicitly serialised **little-endian** using
byte-by-byte encoding — independent of host CPU endianness.

The WAL record format is **unchanged** from Stage 4. Snapshots introduce a
separate format namespace with a different magic number.

---

## Snapshot Metadata: WAL Offset

The most critical field in the snapshot header is the **WAL byte offset**.

```
wal_offset = current size of WAL file at snapshot creation time
```

This value is the byte position in the WAL file past which all records
were written **after** the snapshot. Recovery uses this to seek to the
correct WAL position and replay only the tail.

```
[WAL file]
│
│  ... records at offsets 0 .. wal_offset-1 ...
│
├─── wal_offset ───────────────────────────────
│
│  ... records at offsets wal_offset .. EOF ...
│                   ↑
│           These are replayed during recovery
```

---

## Integrity Protection

The trailing CRC32 checksum covers every byte of the snapshot file except
the checksum field itself. The same CRC32 algorithm used by the WAL
(ISO 3309, reflected polynomial 0xEDB88320) is reused.

The snapshot loader validates:

| Check                              | Error condition                              |
|------------------------------------|----------------------------------------------|
| File minimum size                  | Too small for header + checksum              |
| Magic number                       | Not 0x464B534E                               |
| Format version                     | Not 0x01                                     |
| CRC32 checksum                     | Computed ≠ stored (any corruption)           |
| Record data bounds                 | key_len or val_len reads past checksum field |
| Trailing bytes                     | Extra bytes before checksum                  |

---

## Snapshot Creation Consistency

`KeyValueStore::snapshot()` acquires the **exclusive lock** for its entire
duration. Under this lock:

1. Captures the complete live state via `storage_->get_all()`.
2. Queries the WAL file size via `wal_->file_size()` to obtain the boundary.
3. Writes the snapshot via `SnapshotManager::save()`.

Because the exclusive lock blocks all concurrent `set()`, `del()`, and
`clear()` operations, **no WAL record can be appended between steps 1 and 2**.
The captured key-value state and the WAL byte offset represent the same
consistent logical point in time.

```
snapshot() {
    exclusive_lock lock(mutex_)        ← blocks all writers
    records    = storage_.get_all()    ← consistent state at time T
    wal_offset = wal_.file_size()      ← WAL boundary at time T
    snapshot_manager_.save(wal_offset, records)
}                                      ← lock released
```

This prevents the hazard of:
```
state captured at T1
WAL offset captured at T2  ← if T1 ≠ T2, writes in between are lost
```

---

## Atomic Snapshot Writes

A snapshot is never written directly to the final path. The write process:

1. Build the complete snapshot payload in memory.
2. Write to a temporary file: `<snapshot_path>.tmp`.
3. Flush and close the temporary file.
4. `std::filesystem::rename()` atomically replaces the final path.

```
Write strategy:
  <wal>.snapshot.tmp  (write here)
       ↓ rename (atomic on POSIX)
  <wal>.snapshot      (final path)
```

**Failure handling:**

| Failure point                  | Effect                                             |
|--------------------------------|----------------------------------------------------|
| Cannot create tmp file         | Exception thrown; old snapshot (if any) preserved  |
| Write fails to tmp             | Exception thrown; tmp cleaned up; old preserved    |
| rename() fails                 | Exception thrown; tmp cleaned up; old preserved    |
| Process crash before rename    | Old snapshot survives; tmp may linger              |
| Process crash after rename     | New snapshot is on disk; consistent                |

---

## Recovery Algorithm

On startup, `KeyValueStore::recover()` follows this algorithm:

```
if valid snapshot exists at <wal_path>.snapshot:
    load all key-value pairs from snapshot → storage_
    replay WAL from wal_offset onward
else if snapshot exists but is corrupt:
    LOG WARNING: snapshot corrupt, falling back to WAL recovery
    replay WAL from offset 0 (full recovery)
else (no snapshot):
    replay WAL from offset 0 (full recovery)
```

The final in-memory state is **exactly equivalent** to full WAL replay
from an empty store in all cases.

### Key correctness cases

| Scenario                                    | Recovery result               |
|---------------------------------------------|-------------------------------|
| SET before snapshot                         | From snapshot records         |
| SET after snapshot                          | From WAL tail                 |
| Repeated SET before + after                 | WAL tail value wins           |
| DEL after snapshot                          | Key absent (WAL tail applied) |
| CREATE → snapshot → DEL → CREATE            | Final value (WAL tail)        |
| Empty snapshot + WAL tail with data         | Data from WAL tail            |
| CLEAR after snapshot                        | State wiped, then WAL tail    |

---

## Snapshot Corruption Behavior

**Chosen policy:** fall back to full WAL replay.

If the snapshot file exists but is corrupt (bad magic, version, CRC mismatch,
truncated payload, or extra bytes):

- A warning is printed to `stderr`.
- Recovery falls back to replaying the WAL from offset 0.
- The corrupt snapshot file is **left on disk** (not deleted).
- The correct state is reconstructed from the WAL.
- The next `snapshot()` call will atomically overwrite the corrupt file.

This policy is safe because the WAL always contains a complete independent
history of all operations. Compaction reduces the WAL but never removes
complete records. WAL-only recovery always produces the authoritative state.

**Partial recovery is never performed.** If a snapshot is corrupt, it is
completely ignored — zero records are loaded from it.

---

## Snapshot / Compaction Interaction

Compaction (`KeyValueStore::compact()`) rewrites the WAL from scratch.
After compaction, the WAL starts at byte offset 0 with fresh `SET` records
for all live keys.

Any existing snapshot's `wal_offset` refers to a position in the **old WAL**
that no longer exists. Using that offset after compaction would produce one
of:

- Seeking past EOF → no records replayed (missing post-compaction tail).
- Seeking to the wrong position → replaying garbage.

**Chosen behavior:** `compact()` **deletes the snapshot file** before
calling `wal_->rewrite()`.

```
compact() {
    exclusive_lock lock(mutex_)
    state = storage_.get_all()
    sort(state)
    if snapshot_exists: remove snapshot  ← explicit, documented
    wal_.rewrite(state)                  ← WAL replaced from offset 0
}
```

After compaction:
- Recovery always uses WAL-only replay (from offset 0).
- The compacted WAL already contains the full current state.
- A new `snapshot()` call after compaction produces a valid snapshot that
  correctly references the new compacted WAL.

---

## Snapshot Path Convention

The snapshot file lives at:

```
<wal_path>.snapshot
```

Examples:

| WAL path           | Snapshot path              |
|--------------------|----------------------------|
| `forgekv.wal`      | `forgekv.wal.snapshot`     |
| `/data/store.wal`  | `/data/store.wal.snapshot` |

A single current snapshot is maintained. There are no timestamped or
numbered snapshot files. A new `snapshot()` call atomically replaces the
previous one.

---

## Durability and fsync Limitations

`SnapshotManager::save()` uses standard library buffered I/O flushed via
`stream.flush()`. The rename is atomic at the OS/VFS level on POSIX systems.

However, **neither the snapshot file nor the directory entry is fsynced**.
This matches the existing WAL durability model:

- A power loss after `flush()` but before the OS writes to stable storage
  could, depending on filesystem journaling, leave either the old or the
  new snapshot on disk.
- Typical journaled filesystems (ext4, APFS) protect the rename atomicity
  even without explicit fsync.
- Full power-loss durability would require `fsync()` on the file and
  directory, which is a planned future improvement.

Since the WAL is also not fsynced, the snapshot's durability level is
consistent with the rest of the system.

---

## WAL Additions in Stage 9

### `WAL::replay_from(offset, callback)`

```cpp
[[nodiscard]] ReplayResult
replay_from(std::uint64_t offset,
            std::function<void(const WalRecord&)> callback) const;
```

Seeks to `offset` in the WAL file before beginning record replay. All
existing `replay()` semantics (truncated final record handling, mid-log
corruption detection) apply identically to the tail starting at `offset`.

**Offset validation:**

| Condition          | Behaviour                                    |
|--------------------|----------------------------------------------|
| `offset == 0`      | Equivalent to `replay()`                     |
| `0 < offset < EOF` | Seek to offset; read records from there      |
| `offset == EOF`    | Return empty result (no records to replay)   |
| `offset > EOF`     | Throw `std::runtime_error`                   |

The existing `replay()` method is **unchanged**.

### `WAL::file_size()`

```cpp
[[nodiscard]] std::uint64_t file_size() const noexcept;
```

Returns the current size of the WAL file in bytes, or 0 if the file does
not exist. Used by `snapshot()` to capture the WAL boundary at the exact
moment of checkpoint creation.

---

## API

```cpp
// Create a full-state snapshot. Returns true on success.
// Thread-safe — acquires exclusive lock internally.
bool KeyValueStore::snapshot();

// Compact the WAL. Also deletes any existing snapshot.
// Thread-safe — acquires exclusive lock internally.
void KeyValueStore::compact();
```

No HTTP endpoint is exposed for snapshots in Stage 9. Snapshots are an
internal storage-engine operation.

---

## Files Added / Modified in Stage 9

### New files

| File                            | Purpose                                |
|---------------------------------|----------------------------------------|
| `include/forgekv/snapshot.h`   | SnapshotManager class, constants       |
| `src/snapshot.cpp`             | SnapshotManager implementation         |
| `docs/09-snapshots.md`         | This document                          |

### Modified files

| File                    | Change                                                    |
|-------------------------|-----------------------------------------------------------|
| `include/forgekv/wal.h` | Added `replay_from()` and `file_size()` declarations      |
| `src/wal.cpp`           | Implemented `replay_from()` and `file_size()`             |
| `include/forgekv/kv_store.h` | Added `snapshot()`, `snapshot_manager_` member       |
| `src/kv_store.cpp`      | Snapshot-aware `recover()`, updated `compact()`, `snapshot()` |
| `tests/test_kv_store.cpp` | Added 23 Stage 9 tests (157 total)                     |
| `CMakeLists.txt`        | Version 0.8.0 → 0.9.0, added `snapshot.cpp`              |

---

## Performance

Snapshot creation is **O(N + B)** where N is the number of live keys and
B is the total bytes written. The in-memory state is captured once and
written in a single pass.

Recovery improvement: instead of replaying the full WAL (potentially
thousands of records), only the tail since the last snapshot is traversed.
The improvement scales with the ratio of WAL history to WAL tail size.

No background snapshot threads or automatic periodic snapshots are
implemented in Stage 9.

---

## Future Improvements

- `fsync()` on the snapshot file and directory for full power-loss durability.
- HTTP endpoint for triggering snapshots.
- Automatic periodic snapshots triggered by WAL size or time interval.
- Snapshot compaction coordination (compact → snapshot as a single atomic
  operation with a single exclusive-lock acquisition).
