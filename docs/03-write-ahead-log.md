# 03 — Write-Ahead Log

> **Stage:** 3 (text WAL) and 4 (binary WAL)  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: RAM is Volatile

After Stage 1, ForgeKV's entire state lives in `std::unordered_map`. That map lives in RAM. RAM is volatile: the moment the process exits, crashes, or the machine loses power, every key-value pair is gone forever.

```
Without persistence:

  store.set("account_balance", "10000");
  // ... process crash or SIGKILL ...
  // On restart: the store is empty.
  // The value is gone.
```

For a storage engine, this is the fundamental unsolved problem. Data must outlive the process that holds it.

---

## The Solution: Write-Ahead Log (WAL)

A **Write-Ahead Log** is an append-only file on disk. Every time the store is mutated — every SET and every DELETE — a record of that mutation is written to the WAL **before** the in-memory state is updated.

"Write-ahead" means: the log entry is written first. The in-memory state is updated second. Never the reverse.

```
Mutation request arrives
        │
        ▼
  Write record to WAL file on disk
        │
        ▼ (only after the write succeeds)
  Update in-memory state
        │
        ▼
  Return success to caller
```

If the process crashes between the WAL write and the memory update, the WAL still contains the record. On the next startup, the record is replayed and the memory is updated then.

If the process crashes before the WAL write completes (a partial write), the partial record is detected and discarded. No half-applied mutation enters the store.

---

## Why Append-Only?

The WAL is opened in append mode: new records are always added at the end. The file is never rewritten in place.

This is deliberate:

- **Append is the safest write.** An appended record either fully lands on disk or it does not. Overwriting an existing record can corrupt the old data before the new data is safely written.
- **Sequential writes are fast.** Hard drives and SSDs handle sequential appends better than random writes.
- **Simple recovery.** To reconstruct state, read the file from the beginning and apply records in order. No page management, no B-tree walking.

The tradeoff is that the WAL grows without bound unless compacted. That is addressed in Stage 8.

---

## Stage 3 — Text WAL

The first WAL implementation uses a simple human-readable text format. Each line represents one operation.

Example of what a text WAL might look like:

```
SET name Vishnu
SET age 21
SET city Bengaluru
DELETE age
SET name Vishnu Kumar
```

### Advantages of text WAL

- Easy to inspect with a text editor or `cat`
- Easy to implement and debug
- Helpful during development to verify correctness

### Disadvantages of text WAL

- No fixed structure — parsing requires careful handling of edge cases
- No integrity checking — a truncated or corrupted file is hard to detect reliably
- Inefficient for large keys or values

Text WAL is an intermediate step. It is replaced in Stage 4.

---

## Stage 4 — Binary WAL

Stage 4 replaces the text format with a structured binary format.

### Why Binary?

A binary format allows:

1. **Fixed-width fields.** Record lengths are encoded numerically, not derived from parsing. This makes reading faster and more reliable.
2. **Checksums.** A checksum (e.g., CRC32) is computed over the record content and stored in the header. On replay, recomputing the checksum detects corruption.
3. **Compact storage.** Binary is smaller than text for the same data, especially for numeric values.
4. **Partial write detection.** If a record's stated length does not match available data, or if its checksum fails, it is discarded. A text format makes this harder to detect reliably.

### Conceptual Binary Record Structure

The exact binary format will be finalized during Stage 4 implementation. Conceptually, each record will include:

```
┌────────────────────────────────────────────┐
│  Opcode       (1 byte)                     │  ← SET=1, DELETE=2
│  Key length   (4 bytes, uint32_t)          │
│  Value length (4 bytes, uint32_t)          │  ← 0 for DELETE
│  Checksum     (4 bytes, CRC32)             │  ← over key + value
│  Key          (variable, key_length bytes) │
│  Value        (variable, val_length bytes) │  ← absent for DELETE
└────────────────────────────────────────────┘
```

This is a design sketch. Fields and their sizes may be adjusted during implementation to suit alignment, portability, or practical needs.

### Opcodes

| Opcode | Operation | Meaning                         |
|--------|-----------|----------------------------------|
| 1      | SET       | Insert or update key with value  |
| 2      | DELETE    | Remove key from store            |

GET and EXISTS are read operations. They do not modify state and are never written to the WAL.

---

## Write Path (With WAL)

```
Client calls: SET "name" "Vishnu"
                    │
                    ▼
         Serialize the operation
         into a WAL record
                    │
                    ▼
         Open WAL file (append mode)
                    │
                    ▼
         Write and flush record to disk
         (fsync ensures disk write, not just OS buffer)
                    │
                    ▼
         Update in-memory store:
         store["name"] = "Vishnu"
                    │
                    ▼
         Return success
```

The flush/fsync step is important: without it, the OS may buffer the write in memory, and a crash before the buffer is flushed means the record is lost. `fsync` forces the write to physical storage.

---

## What the WAL Does Not Do

The WAL alone does not:

- Reconstruct state on startup — that is Stage 5 (Crash Recovery)
- Limit its own growth — that is Stage 8 (Compaction)
- Work safely with multiple concurrent writers — that is Stage 7 (Concurrency)

The WAL is a write-only append mechanism. Recovery, compaction, and concurrency are separate concerns.

---

## WAL File Location

The WAL will be stored at a configurable path, defaulting to the current working directory or a configurable data directory. The file name will be something like `forgekv.wal`. The exact naming and location will be finalized at implementation time.

---

*Previous: [02-in-memory-store.md](02-in-memory-store.md)*  
*Next: [04-crash-recovery.md](04-crash-recovery.md)*
