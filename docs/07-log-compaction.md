# 07 — Log Compaction

> **Stage:** 8  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: The WAL Grows Forever

The WAL is an append-only file. Every SET and DELETE appended during ForgeKV's lifetime remains in the file. There is no automatic removal.

Consider this sequence of operations:

```
SET "x" "1"
SET "x" "2"
SET "x" "3"
SET "x" "4"
SET "x" "5"
DELETE "x"
```

The WAL now contains six records. But the net result is that `"x"` does not exist in the store. All six records are obsolete. Yet they remain on disk.

In a real workload, this compounds. A key that is updated thousands of times generates thousands of WAL records. Keys that are created and deleted accumulate as dead weight. The WAL grows without bound.

This causes two problems:

1. **Disk usage.** The WAL consumes ever-increasing storage.
2. **Recovery time.** On startup, every record must be replayed. A large WAL means a slow startup.

---

## What Compaction Does

Compaction rewrites the WAL to contain only the minimum set of operations needed to reconstruct the current store state.

After compaction, the WAL is equivalent to a single SET for every key that currently exists. Deleted keys, superseded values, and historical operations are removed.

### Before compaction (conceptual):

```
WAL file:
  SET  "name"    "Alice"
  SET  "age"     "25"
  SET  "name"    "Bob"
  DELETE "age"
  SET  "name"    "Charlie"
  SET  "city"    "London"
```

### Current in-memory state:

```
  "name" → "Charlie"
  "city" → "London"
```

### After compaction:

```
WAL file (compacted):
  SET  "name"    "Charlie"
  SET  "city"    "London"
```

The compacted WAL is a minimal snapshot expressed as WAL records. It contains exactly the records needed to reconstruct current state, and nothing more.

---

## Why Compaction Is Non-Trivial

Compaction cannot simply truncate the WAL file in place. Several complications arise:

### 1. Atomicity

If ForgeKV crashes mid-compaction, the partially-written compacted WAL must not corrupt the recoverable state. The safe approach:

```
1. Write the compacted records to a temporary file
2. Close and fsync the temporary file
3. Atomically rename the temporary file over the old WAL
```

An atomic rename on POSIX systems (`rename()`) is a single filesystem operation. Either the old file is there, or the new one is — never a partial state.

### 2. Concurrent Access

During compaction, clients may continue writing to the store. Compaction must not block the store for its entire duration (which could be long for a large WAL).

A safe approach: take a snapshot of the current state, write the compacted WAL from that snapshot, then atomically swap the file. New writes that arrived during compaction are appended to the new WAL after the swap.

The exact concurrent compaction strategy will be designed during Stage 8 implementation.

### 3. WAL Integrity

The compacted WAL uses the same binary record format as the original. Recovery does not need to know whether a WAL was compacted — it simply reads and replays records.

---

## Compaction Trigger

Compaction can be triggered:

- **Manually** — via an API call (useful for testing and control)
- **Automatically** — when the WAL exceeds a configurable size threshold

At Stage 8, the trigger policy will be kept simple. Sophisticated adaptive policies are a future improvement.

---

## Effect on Recovery Time

Before compaction:

```
Recovery time ∝ total number of WAL records ever written
```

After compaction:

```
Recovery time ∝ number of currently live keys
```

For a store with 10,000 live keys that has processed 1,000,000 operations, compaction reduces the WAL from 1,000,000 records to 10,000 records — a 99% reduction in recovery time.

---

## Interaction with Snapshots

Stage 9 introduces snapshots. A snapshot is a binary representation of the full in-memory state, not expressed as WAL records. With snapshots, recovery becomes:

```
Load snapshot → replay only post-snapshot WAL records → done
```

Snapshots and compaction are complementary. Compaction shrinks the WAL. Snapshots bypass the WAL for the base state. Together they minimize both disk usage and recovery time.

---

## Compaction Summary

| Property              | Value                                              |
|-----------------------|----------------------------------------------------|
| Input                 | Existing WAL file                                  |
| Output                | Compacted WAL file (same format, fewer records)    |
| Correctness guarantee | Compacted WAL reconstructs identical state         |
| Crash safety          | Write to temp file, atomic rename                  |
| Effect on disk usage  | Significant reduction for long-running stores      |
| Effect on recovery    | Proportional reduction in replay time              |

---

*Previous: [06-concurrency.md](06-concurrency.md)*  
*Next: [08-snapshots.md](08-snapshots.md)*
