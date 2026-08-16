# 04 — Crash Recovery

> **Stage:** 5  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: Crashes Happen

A storage engine cannot assume a clean shutdown. Power cuts happen. Processes receive `SIGKILL`. Machines restart. Operating systems panic. Any of these events will terminate the ForgeKV process without giving it time to save its state.

After Stage 1 (in-memory store only), a crash means total data loss. After Stage 3–4 (WAL), the mutations are recorded on disk — but if the process does not re-read those records on startup, they are useless.

Crash recovery is the mechanism that makes the WAL valuable.

---

## What Recovery Must Do

When ForgeKV starts up, before accepting any new operations, it must:

1. Open the WAL file
2. Read records from the beginning
3. Validate each record
4. Replay valid operations to rebuild in-memory state
5. Stop at the end of the file (or at the first unrecoverable error)
6. Accept new operations only after recovery is complete

The in-memory state after recovery must be identical to the state the store was in at the moment of the last successful write before the crash.

---

## The Recovery Procedure

```
ForgeKV starts
      │
      ▼
Does WAL file exist?
      │
  No ─┼─▶ Start with empty store. Done.
      │
  Yes ▼
      │
Open WAL file for reading
      │
      ▼
┌─────────────────────────────────────────────┐
│              Record Reading Loop            │
│                                             │
│  Read next record                           │
│       │                                     │
│  EOF? ─▶ Exit loop (recovery complete)      │
│       │                                     │
│  Validate record:                           │
│    - Header fields present?                 │
│    - Stated length matches available data?  │
│    - Checksum matches?                      │
│       │                                     │
│  Valid? ─▶ Replay operation into store      │
│       │                                     │
│  Invalid? ─▶ Log warning. Stop recovery.    │
│             (truncate or ignore remainder)  │
└─────────────────────────────────────────────┘
      │
      ▼
Recovery complete.
In-memory state reconstructed.
Begin accepting operations.
```

---

## Handling Problematic Records

The WAL on disk may not be clean. Recovery must handle every failure mode gracefully.

### 1. Empty WAL

The WAL file exists but contains no records. This is normal — it happens if ForgeKV was started, the WAL was created, but no mutations were made before a crash or shutdown.

**Handling:** Start with an empty store. No records to replay.

### 2. Partial Record (Truncated Write)

The process crashed mid-write. The last record in the file is incomplete — the header may be there, but the key or value data is missing.

**Handling:** When attempting to read a record and the file ends before the expected number of bytes, the record is discarded. Everything before it has already been replayed successfully, so the store is consistent up to that point.

### 3. Corrupted Record (Checksum Failure)

The record was written fully, but the data on disk does not match the checksum stored in the header. This can happen due to filesystem bugs, hardware errors, or (rarely) partial writes that appear complete to the OS.

**Handling:** Discard the record. Log a warning. Stop processing the remainder of the WAL (records after a corrupted record may be untrustworthy or may represent a post-corruption partial write).

### 4. Unknown Opcode

The record's opcode field contains a value that ForgeKV does not recognize. This could indicate a version mismatch or corruption.

**Handling:** Treat as an invalid record. Stop replay.

### 5. Valid DELETE on Non-Existent Key

The WAL contains a `DELETE key` record, but the key is not in the current in-memory state (perhaps it was never SET, or was already deleted). This can happen legitimately.

**Handling:** Ignore. A DELETE on a non-existent key is a no-op and does not indicate corruption.

---

## Recovery Example

Suppose the WAL contains the following binary records (described here as text for readability):

```
Record 1: SET  "name"    "Vishnu"
Record 2: SET  "age"     "21"
Record 3: SET  "city"    "Bengaluru"
Record 4: DELETE "age"
Record 5: SET  "name"    "Vishnu Kumar"
Record 6: [partial — process crashed mid-write]
```

Recovery replays Records 1–5, discards Record 6, and produces:

```
In-memory state after recovery:
  "name" → "Vishnu Kumar"
  "city" → "Bengaluru"
```

This is exactly the state the store was in at the time of the last fully committed write.

---

## Durability Guarantee

Recovery provides the following guarantee:

> Any operation that was acknowledged as successful (i.e., its WAL record was fully written and flushed to disk) will be present in the store after recovery.

Operations that were in progress at the time of a crash — not yet written to the WAL, or only partially written — will not appear in the recovered store. From the client's perspective, those operations simply did not complete.

This is the standard durability model for WAL-based storage systems. It is the same model used by PostgreSQL, SQLite, and most other databases.

---

## Interaction with Snapshots

Stage 9 introduces snapshots. With snapshots, recovery becomes:

```
Load latest snapshot
   +
Replay WAL records written after the snapshot
   =
Current state
```

This reduces recovery time when the WAL has grown large. Without snapshots, recovery must replay every record from the beginning of the WAL. With snapshots, it only replays the delta since the last checkpoint.

This interaction is addressed when snapshots are implemented.

---

## What Recovery Does Not Handle

At Stage 5, recovery handles only the WAL. It does not:

- Handle concurrent access during recovery — the store is not open to clients during recovery
- Compact the WAL — that is Stage 8
- Load snapshot files — that is Stage 9
- Evict expired keys — that is Stage 10

Recovery in Stage 5 is deliberately single-purpose: read the WAL, replay valid records, hand a clean in-memory state to the engine.

---

*Previous: [03-write-ahead-log.md](03-write-ahead-log.md)*  
*Next: [05-http-server.md](05-http-server.md)*
