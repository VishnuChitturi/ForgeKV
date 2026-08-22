# 12 — Final Architecture

> **Stage:** All stages contribute to this document.  
> **Status:** 🔲 Planned. This describes the intended final architecture. No implementation exists yet.

> **Stage 20 note:** This is an early planning document describing the intended
> architecture before all stages were implemented. The actual architecture is
> reflected in the source code under `include/forgekv/` and `src/`, and is
> summarized with a Mermaid diagram in `README.md`. The HTTP endpoint list in
> this document is incomplete — see `README.md` for the current full API.

---

## Overview

This document describes the complete planned architecture of ForgeKV as it will exist after all stages are implemented. It is a design reference, not an implementation record.

Components are added one stage at a time. At any given point in development, only the components introduced through the current stage exist. This document describes the eventual whole.

---

## Component Map

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            ForgeKV Process                              │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                         HTTP Server                               │  │
│  │  POST /set   GET /get   POST /delete   GET /exists                │  │
│  │  GET /health             GET /stats                               │  │
│  └──────────────────────────────┬────────────────────────────────────┘  │
│                                 │                                        │
│                                 ▼                                        │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │                          KV Engine                                │  │
│  │               (central coordinator and API surface)              │  │
│  └──┬─────────────┬───────────────┬──────────────┬──────────────────┘  │
│     │             │               │              │                       │
│     ▼             ▼               ▼              ▼                       │
│  ┌──────┐   ┌──────────┐   ┌──────────┐   ┌──────────┐                 │
│  │ In-  │   │  WAL     │   │ Recovery │   │Snapshot  │                 │
│  │Memory│   │ Manager  │   │ Manager  │   │ Manager  │                 │
│  │ Store│   │          │   │          │   │          │                 │
│  │Stage1│   │Stage 3–4 │   │ Stage 5  │   │ Stage 9  │                 │
│  └──────┘   └──────────┘   └──────────┘   └──────────┘                 │
│                                                                         │
│  ┌──────────┐   ┌──────────┐   ┌──────────────────────────────────┐    │
│  │Compaction│   │   TTL    │   │         Statistics               │    │
│  │ Manager  │   │ Manager  │   │  key_count, ops, wal_size,       │    │
│  │ Stage 8  │   │ Stage 10 │   │  uptime, snapshot/compaction info│    │
│  └──────────┘   └──────────┘   └──────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
         │                │                  │
         ▼                ▼                  ▼
    ┌─────────┐    ┌─────────────┐    ┌─────────────┐
    │ WAL file│    │ Snapshot    │    │ Compacted   │
    │(disk)   │    │ file(s)     │    │ WAL file    │
    │         │    │ (disk)      │    │ (disk)      │
    └─────────┘    └─────────────┘    └─────────────┘
```

---

## Component Descriptions

### HTTP Server (Stage 6)

The network boundary. Accepts HTTP requests from clients, parses them, dispatches to the KV Engine, and serializes responses.

Responsibilities:
- Accept TCP connections on a configured port
- Parse HTTP method, path, and request body
- Route requests to the correct engine operation
- Return HTTP responses with appropriate status codes and JSON bodies
- Serve `/health` and `/stats` without touching the data store

Does not contain business logic. It is a thin translation layer between HTTP and the engine API.

### KV Engine (Stages 1–11)

The central coordinator. Owns all other components and exposes the unified API.

Responsibilities:
- Implement SET, GET, DELETE, EXISTS with correct semantics
- Coordinate writes: WAL write first, then memory update
- Trigger compaction when thresholds are met
- Trigger snapshot creation on schedule or on shutdown
- Check TTL expiry on every read
- Accumulate and expose statistics
- Perform recovery on startup before accepting requests

The KV Engine is the only component clients and the HTTP server interact with directly. All other components are internal.

### In-Memory Store (Stage 1–2)

The primary data structure. Holds the live key-value pairs.

Implementation: `std::unordered_map<std::string, Entry>` where `Entry` carries the value and optional TTL expiry timestamp.

Characteristics:
- O(1) average for all operations
- Protected by `std::shared_mutex` (Stage 7)
- State is rebuilt from WAL + snapshot on startup
- Not directly accessed by any component except the KV Engine

### WAL Manager (Stages 3–4)

Manages the write-ahead log file.

Responsibilities:
- Append binary records for SET and DELETE operations
- Flush (fsync) after each record or at configurable intervals
- Provide a reader interface for recovery
- Maintain current WAL file path and size

The WAL Manager does not know about in-memory state. It only knows about records and files.

### Recovery Manager (Stage 5)

Runs once at startup before the engine accepts requests.

Responsibilities:
- Open the WAL file
- Read and validate each record (checksum, length fields)
- Replay valid SET and DELETE operations into the in-memory store
- Handle partial records, corrupt records, and empty WAL gracefully
- After Stage 9: load the latest snapshot first, then replay post-snapshot WAL records

After recovery is complete, the Recovery Manager is idle. It has no ongoing responsibility.

### Snapshot Manager (Stage 9)

Periodically persists the full in-memory state to a binary file.

Responsibilities:
- Serialize all live key-value pairs (including TTL expiry timestamps) to a binary file
- Write atomically: temp file → fsync → rename
- Track the sequence number or timestamp of the snapshot relative to the WAL
- Clean up old snapshot files per retention policy
- Load and deserialize snapshots for the Recovery Manager

### Compaction Manager (Stage 8)

Reduces WAL size by rewriting it to contain only live state.

Responsibilities:
- Read the current in-memory state
- Write a new WAL containing one SET record per live key
- Perform the swap atomically: temp file → fsync → rename
- Append WAL records that arrived during compaction to the new file
- Track compaction statistics (bytes before/after, duration)

### TTL Manager (Stage 10)

Manages key expiration.

Responsibilities:
- Accept expiry timestamps when keys are inserted with TTL
- Provide an `is_expired(key)` check used by GET and EXISTS
- Run a background sweep to evict expired keys and write their DELETE records to the WAL
- Ensure expired keys do not reappear after recovery (expiry timestamp stored in WAL)

### Statistics (Stage 11)

Collects and exposes operational metrics.

Metrics:
- `key_count` — current number of live keys
- `set_count` — total SET operations since startup
- `get_count` — total GET operations since startup
- `delete_count` — total DELETE operations since startup
- `wal_size_bytes` — current WAL file size
- `uptime_seconds` — seconds since startup
- `last_snapshot_time` — timestamp of most recent snapshot
- `last_compaction_time` — timestamp of most recent compaction

All counters are updated atomically (`std::atomic<uint64_t>`).

---

## Write Path (Final)

```
Client: POST /set  {"key":"name", "value":"Vishnu"}
              │
              ▼
        HTTP Server
        parse request
              │
              ▼
          KV Engine
          engine.set("name", "Vishnu")
              │
              ├─── acquire exclusive lock (Stage 7)
              │
              ├─── write WAL record (Stage 3–4)
              │      └── fsync to disk
              │
              ├─── update in-memory store (Stage 1)
              │
              ├─── increment set_count (Stage 11)
              │
              └─── release exclusive lock
              │
              ▼
        HTTP Server
        respond: 200 {"status":"ok"}
```

---

## Read Path (Final)

```
Client: GET /get?key=name
              │
              ▼
        HTTP Server
        parse request
              │
              ▼
          KV Engine
          engine.get("name")
              │
              ├─── acquire shared lock (Stage 7)
              │
              ├─── look up key in in-memory store
              │
              ├─── check TTL expiry (Stage 10)
              │      └── if expired: return not_found
              │
              ├─── increment get_count (Stage 11)
              │
              └─── release shared lock
              │
              ▼
        HTTP Server
        respond: 200 {"key":"name", "value":"Vishnu"}
             or: 404 {"error":"key not found"}
```

---

## Startup Sequence (Final)

```
ForgeKV starts
      │
      ▼
Recovery Manager runs:
      │
      ├── Does a snapshot exist?
      │       Yes → load snapshot → in-memory store populated
      │       No  → start with empty store
      │
      ├── Open WAL file
      ├── Skip records already covered by snapshot (if any)
      ├── Replay remaining valid WAL records
      └── Recovery complete
      │
      ▼
HTTP Server starts listening
Accept client requests
```

---

## Disk Layout (Final)

```
<data_dir>/
├── forgekv.wal              ← active write-ahead log
├── forgekv.snapshot.001     ← latest snapshot
├── forgekv.snapshot.000     ← previous snapshot (retention)
└── forgekv.wal.tmp          ← temporary file during compaction (transient)
```

File names and paths are configurable. The layout shown is the default.

---

## What This Architecture Demonstrates

A developer who has built this system can explain:

1. Why data structures alone are not storage engines (Stage 1–2)
2. Why a WAL is the standard durability primitive (Stage 3–4)
3. How crash recovery is implemented correctly, including failure cases (Stage 5)
4. How an HTTP API maps to a storage engine (Stage 6)
5. Why readers-writer locks are the right concurrency model for this workload (Stage 7)
6. Why and how WAL compaction works (Stage 8)
7. Why snapshots reduce recovery time (Stage 9)
8. How TTL is implemented without external timers (Stage 10)
9. What operational metrics matter and how to expose them (Stage 11)
10. How to build a benchmark suite that produces real results (Stage 12)
11. What it takes to thoroughly test a storage system (Stage 13)

That is the purpose of ForgeKV.

---

*Previous: [11-testing.md](11-testing.md)*  
*Back to: [README.md](../README.md)*
