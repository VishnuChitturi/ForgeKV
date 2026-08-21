# Stage 8 — Log Compaction

## Why Compaction is Necessary

The WAL is append-only. Every `SET`, `DEL`, and `CLEAR` adds a new record.
Over time this produces a file full of historical, redundant entries:

```
SET user = Vishnu      ← obsolete
SET user = Rahul       ← obsolete
SET user = Alex        ← live
SET age  = 20          ← obsolete
SET age  = 21          ← live
DEL old_key            ← key no longer exists
SET city = Bangalore   ← obsolete
SET city = Mysore      ← live
```

The WAL grows indefinitely even though the current state is only three keys:

```
user = Alex
age  = 21
city = Mysore
```

This causes two concrete problems:

1. **Disk growth**: the WAL consumes far more space than the current data.
2. **Slow recovery**: replaying a long WAL on startup is slower than necessary.

Compaction solves both by rewriting the WAL to contain only the current state.

---

## What Compaction Does

`KeyValueStore::compact()` rewrites the WAL so that it contains exactly one
`SET` record for every currently live key. Deleted keys are not written.

After compaction, replaying the compacted WAL reconstructs exactly the same
logical state as before compaction.

### Before compaction

```
SET A = 1
SET B = 2
SET A = 3
DEL B
SET C = 4
SET A = 5
```

Current state: `A=5`, `C=4`.

### After compaction

```
SET A = 5
SET C = 4
```

Records are written in **lexicographic key order** for determinism and
reproducible tests.

The binary record format (magic, version, opcode, key_len, val_len, payload,
CRC32) is **unchanged**. Compaction only changes which records exist; it does
not change how each record is encoded.

---

## Atomic Temporary-File Replacement

Compaction never truncates the existing WAL first. Truncating then rewriting
would create a window where a crash destroys the only durable copy of the data.

Instead, an atomic replacement strategy is used:

```
1. Acquire exclusive lock (KeyValueStore mutex_)
2. Snapshot current live state from in-memory storage
3. Sort snapshot by key (lexicographic)
4. Create temporary file in same directory as WAL:
       <wal_path>.compact.tmp
5. Write all SET records to temporary file
6. Flush and close temporary file
7. std::filesystem::rename(tmp, wal_path)   ← atomic on POSIX
8. Reopen WAL output stream on new file
9. Release exclusive lock
```

The temporary file is placed in the **same directory** as the WAL so that
`rename()` is guaranteed to be on the same filesystem partition. On POSIX
systems `rename()` is an atomic operation: a reader either sees the old WAL
or the new WAL, never a half-written state.

### Failure handling

| Failure point                        | Effect                                          |
|--------------------------------------|-------------------------------------------------|
| Cannot create temp file              | Exception thrown; original WAL untouched        |
| Write fails to temp file             | Exception thrown; original WAL untouched        |
| `rename()` fails                     | Exception thrown; temp file cleaned up; original WAL untouched |
| `rename()` succeeds, reopen fails    | Exception thrown; compacted WAL on disk but stream invalid — store unusable |
| Process crashes before `rename()`   | Original WAL survives; temp file may linger on disk |
| Process crashes after `rename()`    | Compacted WAL is on disk; consistent state      |

In all failure cases before the rename, the in-memory state is unchanged.

---

## Interaction with KeyValueStore Locking

Compaction is a **write operation** and acquires the exclusive lock
(`std::unique_lock<std::shared_mutex>`) for its full duration:

```
compact() {
    exclusive_lock lock(mutex_)     ← blocks all readers and writers
    snapshot = storage_.get_all()   ← stable snapshot under lock
    sort(snapshot)
    wal_.rewrite(snapshot)          ← atomic file replace + reopen
}                                   ← lock released
```

This ensures:

- No concurrent `set()`, `del()`, or `clear()` can modify the state while the
  snapshot is being taken or the WAL is being replaced.
- No concurrent WAL append can interleave with the atomic file replace.
- Readers cannot observe an inconsistent intermediate state.

No second mutex is added. The existing `KeyValueStore::mutex_` provides all
necessary synchronization.

---

## WAL Reopening

After `rename()` atomically replaces the WAL file, the old `std::ofstream`
still holds a reference to the **old inode** on POSIX systems. Any subsequent
`append_set()` or `append_del()` would silently write to the now-unlinked
old file — data that would be lost on the next open.

`WAL::rewrite()` explicitly closes and reopens `stream_` on the new path
after the rename:

```cpp
stream_.close();
stream_.open(path_, std::ios::binary | std::ios::app);
```

This is verified by test `s8_writes_after_compaction_persist`:

```
create state → compact → SET/DELETE → destroy → recreate → recover
```

The post-compaction `SET`/`DELETE` operations must survive recovery. If the
stream reopen were omitted, those operations would be written to the old inode
and lost after restart.

---

## Recovery Behavior

The compacted WAL uses the same binary record format as normal WAL records.
Recovery (`WAL::replay()` + `Recovery::run()`) is unchanged — it replays
all complete valid records in file order.

After compaction, recovery is faster because there are fewer records to replay.
The recovered state is identical to the pre-compaction state.

---

## Durability and fsync Limitations

`WAL::rewrite()` uses standard library buffered I/O flushed via
`stream.flush()`. The rename operation is atomic at the OS/VFS level on POSIX
systems.

However, **neither the WAL file nor the directory entry is fsynced**. This
matches the existing WAL durability model:

- A power loss after `flush()` but before the OS writes the data to stable
  storage could, depending on filesystem journaling, leave either the old or
  the new WAL on disk.
- Typical journaled filesystems (ext4, APFS, HFS+) protect the rename
  atomicity even without explicit fsync.
- Full power-loss durability would require `fsync()` on the file and directory
  descriptor, which is a planned future improvement but is not implemented in
  Stage 8.

This limitation is the same as for normal WAL record writes.

---

## Compaction Complexity

```
O(N + B)
```

Where `N` is the number of live keys and `B` is the total bytes written.

The current live state is already in RAM (in `InMemoryStorage`). Compaction
reads it once via `Storage::get_all()`, sorts it once (`O(N log N)`), and
writes one record per key (`O(N + B)`). The historical WAL file is never
rescanned.

---

## API

```cpp
// Compact the WAL to only the current live state.
// Thread-safe (acquires exclusive lock internally).
void KeyValueStore::compact();
```

No HTTP endpoint is exposed for compaction in Stage 8.
Compaction is an internal storage-engine operation.

---

## New Methods Added in Stage 8

### `Storage::get_all()` (interface + InMemoryStorage)

```cpp
[[nodiscard]] virtual std::vector<std::pair<std::string, std::string>>
get_all() const = 0;
```

Returns a snapshot of all live key-value pairs. Used exclusively by
`KeyValueStore::compact()` to build the compaction snapshot. The result is
unsorted; `compact()` sorts by key before passing to `WAL::rewrite()`.

### `WAL::rewrite()`

```cpp
void rewrite(const std::vector<std::pair<std::string, std::string>>& snapshot);
```

Writes the snapshot to a temp file, atomically renames it over the WAL, and
reopens the output stream. Called only by `KeyValueStore::compact()` under
the exclusive lock.
