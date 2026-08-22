# ForgeKV

A persistent, concurrent key-value storage engine built from scratch in C++20.

> **Status:** Stage 12 — Benchmarking. KV engine, HTTP server, WAL, snapshots, compaction, TTL, statistics, and benchmark suite all implemented.
> See the [Development Roadmap](#development-roadmap) for the full history.

---

## Table of Contents

1. [What is ForgeKV?](#what-is-forgekv)
2. [Motivation](#motivation)
3. [What is a Key-Value Store?](#what-is-a-key-value-store)
4. [Why In-Memory Storage?](#why-in-memory-storage)
5. [Why Persistence is Needed](#why-persistence-is-needed)
6. [High-Level Architecture](#high-level-architecture)
7. [Development Roadmap](#development-roadmap)
8. [Planned Features](#planned-features)
9. [Project Structure](#project-structure)
10. [Technology Stack](#technology-stack)
11. [Building the Project](#building-the-project)
12. [Testing Strategy](#testing-strategy)
13. [Benchmarking Strategy](#benchmarking-strategy)
14. [Learning Objectives](#learning-objectives)
15. [Future Improvements](#future-improvements)

---

## What is ForgeKV?

ForgeKV is a key-value storage engine written in C++20, built entirely from first principles as a learning and portfolio project. It starts as a simple in-memory hash map and evolves — stage by stage — into a networked, crash-safe, concurrent storage system.

This is not a production replacement for Redis, RocksDB, or any mature database. The goal is to demonstrate a deep understanding of how persistent storage systems actually work by building one from the ground up.

---

## Motivation

Most developers interact with storage systems at the API level: call `SET`, call `GET`, receive a result. But what happens inside? How does data survive a power loss? How does the system reconstruct its state after a crash? How does it serve hundreds of clients simultaneously without corrupting data?

ForgeKV exists to answer those questions through implementation. Every design decision — from the in-memory data structure to the binary WAL record format to the concurrency model — is made consciously and documented explicitly.

Building ForgeKV means being able to explain, at a technical interview, how a storage engine works from the lowest level up.

---

## What is a Key-Value Store?

A key-value store is the simplest useful form of a database. It maintains a mapping from keys to values, where:

- A **key** is a unique identifier (e.g., a string like `"username"`)
- A **value** is the data associated with that key (e.g., `"vishnu"`)

The core operations are:

| Operation | Description                                   |
|-----------|-----------------------------------------------|
| `SET`     | Store or update a value for a given key       |
| `GET`     | Retrieve the value for a given key            |
| `DELETE`  | Remove a key-value pair                       |
| `EXISTS`  | Check whether a key is present                |

This maps naturally to a hash map: `std::unordered_map<std::string, std::string>` in C++.

Real-world examples of key-value stores include Redis, DynamoDB, etcd, and RocksDB.

---

## Why In-Memory Storage?

RAM is orders of magnitude faster than disk. A CPU can access RAM in nanoseconds; a disk seek takes microseconds to milliseconds. Keeping the active dataset in memory means operations are fast by default.

ForgeKV starts with an in-memory `std::unordered_map` because:

1. It is the simplest correct implementation of the core semantics.
2. Average `O(1)` complexity for `GET`, `SET`, `DELETE`, and `EXISTS`.
3. It creates a clean baseline to build persistence on top of.

The in-memory store is not the final design — it is the foundation.

---

## Why Persistence is Needed

RAM is **volatile**. When a process exits, crashes, or the machine restarts, everything in memory is gone. For a storage engine, this is unacceptable.

ForgeKV will address this with a **Write-Ahead Log (WAL)**. Before any mutation is applied to the in-memory state, a record of that mutation is appended to a log file on disk. On restart, the log is replayed to reconstruct the in-memory state.

```
Without persistence:
  Process crash → all data lost

With WAL:
  Process crash → restart → replay WAL → state restored
```

Persistence is what separates a storage engine from a plain data structure.

---

## High-Level Architecture

> **Note:** This is the planned final architecture. Only the in-memory store is currently being built.

```
Client
  │
  ▼
HTTP Server              ← exposes REST API (Stage 6)
  │
  ▼
KV Engine                ← central coordinator
  │
  ├── In-Memory State    ← std::unordered_map (Stage 1)
  │
  ├── WAL Manager        ← write-ahead log (Stage 3–4)
  │
  ├── Recovery Manager   ← WAL replay on startup (Stage 5)
  │
  ├── Snapshot Manager   ← periodic state checkpoints (Stage 9)
  │
  ├── Compaction Manager ← WAL space reclamation (Stage 8)
  │
  ├── TTL Manager        ← key expiration (Stage 10)
  │
  └── Statistics         ← operational metrics (Stage 11)
```

Each component is introduced at the appropriate stage. The architecture is designed so each layer adds capability without breaking the previous one.

---

## Development Roadmap

| Stage | Title                   | Status      | Description                                              |
|-------|-------------------------|-------------|----------------------------------------------------------|
| 0     | Project Foundation      | ✅ Current  | Repo structure, build system, documentation              |
| 1     | In-Memory Store         | 🔲 Planned  | `KeyValueStore` backed by `unordered_map`                |
| 2     | Storage Abstraction     | 🔲 Planned  | Decouple engine from concrete map implementation         |
| 3     | Write-Ahead Log (text)  | 🔲 Planned  | Append-only log for mutations                            |
| 4     | Binary WAL              | 🔲 Planned  | Structured binary record format with checksums           |
| 5     | Crash Recovery          | 🔲 Planned  | Replay WAL on startup to reconstruct state               |
| 6     | HTTP Server             | 🔲 Planned  | REST API over HTTP                                       |
| 7     | Concurrency             | 🔲 Planned  | Thread-safe access with `std::shared_mutex`              |
| 8     | Log Compaction          | 🔲 Planned  | Reclaim WAL space by removing obsolete entries           |
| 9     | Snapshots               | 🔲 Planned  | Periodic full-state checkpoints                          |
| 10    | TTL / Expiration        | 🔲 Planned  | Key expiry with background cleanup                       |
| 11    | Statistics              | 🔲 Planned  | Operational metrics and observability                    |
| 12    | Benchmarking            | ✅ Complete | Throughput, latency, and resource benchmarks             |
| 13    | Testing                 | 🔲 Planned  | Unit, integration, persistence, concurrency, stress tests|

See the `docs/` directory for detailed design notes on each stage.

---

## Planned Features

All features below are **planned**. None are currently implemented.

- **Persistence** via Write-Ahead Log
- **Binary WAL** with structured records and checksums
- **Crash recovery** — replay WAL on startup
- **HTTP REST API** — `SET`, `GET`, `DELETE`, `EXISTS`, `health`, `stats`
- **Concurrency** — thread-safe reads and writes
- **Log compaction** — reclaim disk space, speed up recovery
- **Snapshots** — periodic full-state checkpoints
- **TTL** — optional key expiration
- **Statistics** — key count, operation counts, WAL size, uptime
- **Benchmarks** — measured throughput, latency percentiles
- **Test suite** — unit, integration, crash, concurrency, stress tests

---

## Project Structure

```
ForgeKV/
├── README.md              ← This file
├── CMakeLists.txt         ← Build system configuration
├── .gitignore
│
├── include/               ← Public header files (populated from Stage 1)
├── src/                   ← Implementation files (populated from Stage 1)
├── tests/                 ← Test suite (Stage 13)
├── benchmarks/            ← Benchmark suite (Stage 12)
├── examples/              ← Usage examples (future)
│
└── docs/
    ├── 01-project-overview.md
    ├── 02-in-memory-store.md
    ├── 03-write-ahead-log.md
    ├── 04-crash-recovery.md
    ├── 05-http-server.md
    ├── 06-concurrency.md
    ├── 07-log-compaction.md
    ├── 08-snapshots.md
    ├── 09-ttl.md
    ├── 10-benchmarking.md
    ├── 11-testing.md
    └── 12-final-architecture.md
```

---

## Technology Stack

| Component     | Choice                     | Reason                                              |
|---------------|----------------------------|-----------------------------------------------------|
| Language      | C++20                      | Systems-level control, modern standard library      |
| Build system  | CMake 3.20+                | Cross-platform, industry standard                   |
| Testing       | To be selected at Stage 13 | Will evaluate GoogleTest or Catch2                  |
| HTTP server   | To be selected at Stage 6  | Will evaluate cpp-httplib or minimal from-scratch   |
| Dependencies  | Minimal by design          | Each addition must justify its presence             |

External libraries are introduced only when they provide clear value over a from-scratch implementation.

---

## Building the Project

**Prerequisites:** CMake 3.20+, a C++20-capable compiler (GCC 11+, Clang 13+, or Apple Clang 14+).

```bash
git clone https://github.com/yourusername/ForgeKV.git
cd ForgeKV
cmake -B build
cmake --build build
```

At Stage 0 there are no build targets. CMake configuration should succeed and print a status summary.

---

## Testing Strategy

> **Status: Planned (Stage 13)**

ForgeKV will include a comprehensive test suite covering:

- **Unit tests** — individual components in isolation (store, WAL, recovery, TTL)
- **Integration tests** — components working together end-to-end
- **Persistence tests** — data survives process restart
- **WAL tests** — correct record format, ordering, and flushing
- **Corruption tests** — partial records and checksum failures handled gracefully
- **Crash recovery tests** — state correctly reconstructed after simulated crash
- **Concurrency tests** — no data races under multi-threaded access
- **Stress tests** — behavior under sustained high load

The test suite will be introduced progressively, with tests added as each stage is implemented.

---

## Benchmarking Strategy

> **Status: Implemented (Stage 12)**

ForgeKV includes a benchmark suite measuring actual, reproducible performance on real hardware. No numbers are invented or estimated in advance.

Metrics measured:

- GET / SET / DELETE throughput (operations per second)
- Average, P50, P95, and P99 latency
- WAL file size vs. operation count
- Compaction time and space savings
- Snapshot time and file size
- Concurrency scalability (1, 2, 4, 8 threads)
- HTTP-level throughput (GET, PUT, mixed)

Results are obtained by running `./build/forgekv_benchmark`. See `docs/12-benchmarking.md` for full details.

---

## Learning Objectives

By building ForgeKV, the following topics are explored hands-on:

| Topic                     | Stage    |
|---------------------------|----------|
| C++ data structures       | 1        |
| Storage abstraction       | 2        |
| File I/O and durability   | 3        |
| Binary serialization      | 4        |
| Crash recovery            | 5        |
| HTTP and networking       | 6        |
| Threads and mutexes       | 7        |
| Log compaction            | 8        |
| Snapshots/checkpointing   | 9        |
| TTL and expiration        | 10       |
| Observability             | 11       |
| Performance benchmarking  | 12       |
| Systems testing           | 13       |

---

## Future Improvements

Ideas beyond the current roadmap, considered only after Stage 13 is complete:

- Replace `std::unordered_map` with a custom hash table
- LSM-tree or B-tree storage backend as an alternative engine
- Replication across multiple nodes
- Write batching and atomic multi-key transactions
- Bloom filters for negative-lookup optimization
- Memory-mapped I/O for the WAL
- gRPC or binary protocol as an alternative to HTTP
- Persistent memory (pmem) support

These are aspirational. ForgeKV will not claim to support them until they are built and tested.

---

*ForgeKV is a learning project by Vishnu. Built from first principles, one stage at a time.*
