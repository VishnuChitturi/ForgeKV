# 08 — Snapshots

> **Stage:** 9  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: WAL Replay Has a Cost

Crash recovery works by replaying the WAL from the beginning. After Stage 8 (Compaction), the WAL is kept reasonably sized. But even with compaction, a long-running store accumulates WAL records between compaction runs.

If the WAL grows large between compactions — thousands or millions of records — then recovery on startup means reading and processing all of them before accepting any client requests. For a large dataset, this delay can become noticeable.

Snapshots solve this by providing a recovery shortcut.

---

## What a Snapshot Is

A snapshot (also called a checkpoint) is a file containing the full current state of the in-memory store, serialized to disk at a point in time.

Rather than a log of operations (like the WAL), a snapshot is a complete map: every key and its current value, written once.

```
Snapshot file at time T:
  "name"    → "Vishnu Kumar"
  "city"    → "Bengaluru"
  "session" → "abc123"
  ...all other live keys...
```

A snapshot is not a replacement for the WAL. It is a complement.

---

## The Recovery Model with Snapshots

Without snapshots:

```
Recovery: replay ALL WAL records from the beginning
```

With snapshots:

```
Recovery:
  1. Load the latest snapshot
         +
  2. Replay only WAL records written AFTER the snapshot
         =
  3. Current state
```

This bounds recovery time. No matter how long ForgeKV has been running, recovery only needs to replay the delta since the last snapshot.

```
Timeline:
  ──────────────────────────────────────────────────────▶ time

  [snapshot S1]──[WAL records]──[snapshot S2]──[WAL records]──[CRASH]
                                                               │
                  Recovery starts here ◀───────────────────────┘
                  Load S2, replay only post-S2 WAL records
```

---

## Snapshot File Format

The exact binary format will be designed during Stage 9 implementation. Conceptually, the snapshot file contains:

```
┌─────────────────────────────────────────────────────┐
│  Snapshot header                                    │
│    - magic number (identifies file type)            │
│    - format version                                 │
│    - timestamp                                      │
│    - key count                                      │
│    - overall checksum (or per-record checksums)     │
├─────────────────────────────────────────────────────┤
│  Record 1:                                          │
│    - key length (uint32_t)                          │
│    - value length (uint32_t)                        │
│    - key (variable)                                 │
│    - value (variable)                               │
├─────────────────────────────────────────────────────┤
│  Record 2: ...                                      │
├─────────────────────────────────────────────────────┤
│  ...                                                │
└─────────────────────────────────────────────────────┘
```

The exact layout will be finalized when implemented.

---

## Snapshot Safety

Writing a snapshot must be safe with respect to crashes and concurrent access.

### Crash during snapshot write

If ForgeKV crashes while writing a snapshot, the partial snapshot file must not corrupt recovery.

Safe approach:
```
1. Write snapshot to a temporary file
2. fsync the temporary file
3. Atomically rename the temporary file to the final snapshot path
```

The rename is atomic on POSIX systems. If the crash happens before the rename, the temporary file is incomplete and ignored. If it happens after, the new snapshot is complete and valid.

### Concurrent access during snapshot write

A snapshot represents the store at a specific point in time. The simplest approach: take a read lock on the store for the duration of the snapshot write, blocking writers temporarily. For a large store, this may pause writes for a noticeable duration.

A more sophisticated approach is copy-on-write or a versioned snapshot that allows writes to continue. This is a potential future optimization.

---

## Snapshot Trigger

Snapshots can be triggered:

- **On a time schedule** — every N minutes
- **On a write count threshold** — every N mutations since the last snapshot
- **Manually** — via an API call
- **On clean shutdown** — to minimize WAL replay on the next startup

The trigger policy will be configurable. At Stage 9, a simple interval-based trigger will be implemented.

---

## Snapshot + WAL Together

Snapshots and the WAL work as a pair:

| Mechanism   | Purpose                                    |
|-------------|--------------------------------------------|
| WAL         | Durability for every individual mutation    |
| Snapshot    | Fast recovery baseline                     |
| Compaction  | Bound WAL size between snapshots           |

The WAL is still required even with snapshots. Without the WAL, mutations between the last snapshot and a crash would be lost. The WAL covers the delta; the snapshot covers the base.

---

## Old Snapshot Retention

After a new snapshot is written, the previous snapshot can be deleted. Keeping the last one or two snapshots is a safeguard: if the newest snapshot turns out to be corrupt, recovery can fall back to the previous one.

Snapshot retention policy will be configurable. The default will be to keep the last N snapshots (N to be decided at implementation time).

---

## Effect on Recovery

| Scenario                        | Recovery procedure                            |
|---------------------------------|-----------------------------------------------|
| No snapshot, small WAL          | Replay WAL from beginning — fast              |
| No snapshot, large WAL          | Replay WAL from beginning — slow              |
| Snapshot exists, small delta    | Load snapshot + replay short WAL — very fast  |
| Snapshot exists, large delta    | Load snapshot + replay longer WAL — moderate  |
| Snapshot corrupt, WAL intact    | Fall back to previous snapshot or full replay |

---

*Previous: [07-log-compaction.md](07-log-compaction.md)*  
*Next: [09-ttl.md](09-ttl.md)*
