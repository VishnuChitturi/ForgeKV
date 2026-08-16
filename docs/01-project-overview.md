# 01 — Project Overview

> **Stage:** 0 — Foundation  
> **Status:** Documentation only. No implementation exists yet.

---

## What is ForgeKV?

ForgeKV is a key-value storage engine built from scratch in C++20. It begins as the simplest possible thing — a hash map in memory — and grows, one stage at a time, into a networked, persistent, concurrent storage system.

It is not a production database. It is a deliberately constructed learning project that forces real engagement with the problems that every storage system must solve: durability, recovery, concurrency, and space management.

---

## Motivation

When you call `SET key value` on Redis, something happens. A lot of somethings, actually. Redis checks for expiry. It writes to its append-only file (AOF). It handles concurrent clients. It manages memory. It recovers on restart.

Most software developers never think about this layer. ForgeKV is built to make that invisible layer visible. Every mechanism — the log, the binary record format, the read/write lock, the compaction algorithm — is implemented explicitly and documented.

The end goal is the ability to sit in a systems design interview and explain, from first principles, how a storage engine works. Not from memory. From having built one.

---

## Core Concept

The fundamental idea is straightforward:

```
The current database state lives in memory.
Disk holds a log of all mutations.
On restart, replay the log to rebuild memory.
```

RAM is fast but volatile. Disk is slow but durable. The architecture of ForgeKV is built around reconciling that tradeoff.

---

## Architecture Overview

> **Note:** This is the planned final architecture. At Stage 0 none of these components exist yet.

```
Client (HTTP)
      │
      ▼
┌─────────────────────────────────────────────────────────┐
│                      HTTP Server                        │
│              (REST API — Stage 6)                       │
└───────────────────────────┬─────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────┐
│                       KV Engine                         │
│                  (central coordinator)                  │
│                                                         │
│  ┌──────────────────┐   ┌──────────────────────────┐    │
│  │  In-Memory State │   │       WAL Manager        │    │
│  │  (unordered_map) │   │  (write-ahead log)       │    │
│  │  Stage 1         │   │  Stages 3–4              │    │
│  └──────────────────┘   └──────────────────────────┘    │
│                                                         │
│  ┌──────────────────┐   ┌──────────────────────────┐    │
│  │ Recovery Manager │   │    Snapshot Manager      │    │
│  │  Stage 5         │   │    Stage 9               │    │
│  └──────────────────┘   └──────────────────────────┘    │
│                                                         │
│  ┌──────────────────┐   ┌──────────────────────────┐    │
│  │Compaction Manager│   │      TTL Manager         │    │
│  │  Stage 8         │   │      Stage 10            │    │
│  └──────────────────┘   └──────────────────────────┘    │
│                                                         │
│  ┌──────────────────┐                                   │
│  │   Statistics     │                                   │
│  │   Stage 11       │                                   │
│  └──────────────────┘                                   │
└─────────────────────────────────────────────────────────┘
```

Each component is added at a specific stage. No component is introduced before it is needed.

---

## Development Stages

### Stage 0 — Project Foundation ✅ Current

Repository structure, build system, and documentation. No database code.

### Stage 1 — In-Memory Store 🔲 Planned

A `KeyValueStore` class backed by `std::unordered_map<std::string, std::string>`. Supports SET, GET, DELETE, EXISTS. State is RAM-only and lost on process exit.

### Stage 2 — Storage Abstraction 🔲 Planned

Introduce a clean interface between the KV engine and its underlying storage. The rest of the system stops depending directly on `unordered_map`, making the design extensible.

### Stage 3 — Write-Ahead Log (Text) 🔲 Planned

Before any mutation touches memory, write a human-readable record to an append-only log file on disk. This makes mutations durable.

### Stage 4 — Binary WAL 🔲 Planned

Replace the text log with a structured binary record format. Binary is more compact, faster to write and parse, and enables proper checksumming for integrity validation.

### Stage 5 — Crash Recovery 🔲 Planned

On startup, open the WAL, validate each record, and replay valid operations to reconstruct in-memory state. Handle partial writes, corruption, and empty logs gracefully.

### Stage 6 — HTTP Server 🔲 Planned

Expose the engine as an HTTP service. Clients can issue REST requests (POST /set, GET /get, etc.) instead of calling C++ directly.

### Stage 7 — Concurrency 🔲 Planned

Allow multiple clients and threads to operate simultaneously. Use `std::shared_mutex` to distinguish reads (concurrent-safe) from writes (require exclusive access). Guarantee no data races.

### Stage 8 — Log Compaction 🔲 Planned

The WAL accumulates entries indefinitely. Compaction rewrites it to keep only the minimum information needed to reconstruct current state, discarding obsolete records and reclaiming disk space.

### Stage 9 — Snapshots 🔲 Planned

Periodically snapshot the full in-memory state to disk. Recovery becomes: load the latest snapshot, then replay only the WAL entries written after that snapshot. This bounds recovery time regardless of WAL age.

### Stage 10 — TTL / Expiration 🔲 Planned

Keys can be assigned a time-to-live. After the TTL expires, the key becomes invisible. A background thread eventually evicts expired entries and removes their WAL records.

### Stage 11 — Statistics 🔲 Planned

Expose operational metrics: key count, operation counters, WAL file size, uptime, and compaction/snapshot state. Only metrics that can actually be measured are included.

### Stage 12 — Benchmarking 🔲 Planned

A benchmark suite measuring real, reproducible performance: throughput in operations per second, latency percentiles (P50/P95/P99), memory usage, WAL growth, recovery time, compaction time. No fabricated numbers.

### Stage 13 — Testing 🔲 Planned

A comprehensive test suite: unit tests per component, integration tests for multi-component flows, persistence tests, WAL format tests, corruption/recovery tests, concurrency tests, and stress tests.

---

## Project Philosophy

**Build to understand, not to ship.**

Every design decision is made deliberately. When a simpler approach exists, it is used first. Complexity is added only when it solves a real problem. No stage introduces a feature that is not immediately motivated by a concrete limitation of the previous stage.

**Document as you build.**

Each feature has a corresponding design document in `docs/` written before or alongside the implementation. The documentation explains the "why", not just the "what".

**Measure, don't estimate.**

No performance claims are made until they are backed by a reproducible measurement from the actual implementation. The benchmarking section contains only real numbers.

---

## Technology

| Decision     | Choice          | Reason                                                  |
|--------------|-----------------|---------------------------------------------------------|
| Language     | C++20           | Systems-level control, modern STL, range-based features |
| Build system | CMake 3.20+     | Cross-platform, industry standard                       |
| Dependencies | Minimal         | Each library must justify its addition                  |
| Testing      | TBD (Stage 13)  | GoogleTest or Catch2 will be evaluated at implementation |
| HTTP         | TBD (Stage 6)   | cpp-httplib or from-scratch will be evaluated           |

---

## What This Project Is Not

- Not a Redis clone
- Not a RocksDB replacement
- Not production software
- Not a tutorial or guided course

It is a demonstration that the author can take a storage problem and build a real solution to it, from first principles, in a real language, and explain every layer of it.

---

*Next: [02-in-memory-store.md](02-in-memory-store.md)*
