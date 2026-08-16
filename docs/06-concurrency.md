# 06 — Concurrency

> **Stage:** 7  
> **Status:** 🔲 Planned. This document describes the design before implementation begins.

---

## The Problem: Multiple Clients at Once

Through Stage 6, ForgeKV has an HTTP server — but that server handles operations one at a time relative to the shared store. If two clients send requests simultaneously, the behavior is undefined.

The root issue is **shared mutable state**: the in-memory `unordered_map` and the WAL file are shared resources. Multiple threads accessing them without coordination can corrupt data.

---

## What a Data Race Is

A data race occurs when two or more threads access the same memory location concurrently, at least one access is a write, and there is no synchronization between them.

In C++, a data race is undefined behavior. The program may produce wrong results, silently corrupt the store, or crash.

Example of a problematic scenario without synchronization:

```
Thread A: SET "balance" "1000"
Thread B: GET "balance"

If these execute simultaneously without locking:
  - Thread B might read a partially-written value
  - Thread A's write might be lost due to internal rehashing
  - The unordered_map itself may corrupt its internal structure
```

---

## The Solution: Mutual Exclusion

The solution is a lock — a synchronization primitive that ensures only permitted threads can access the shared resource at any given time.

C++17/C++20 provides several options in `<shared_mutex>` and `<mutex>`:

| Primitive            | Behavior                                           |
|---------------------|----------------------------------------------------|
| `std::mutex`         | Exclusive access for one thread at a time          |
| `std::shared_mutex`  | Multiple concurrent readers OR one exclusive writer |
| `std::lock_guard`    | RAII wrapper for exclusive lock                    |
| `std::shared_lock`   | RAII wrapper for shared (read) lock                |
| `std::unique_lock`   | RAII wrapper for exclusive lock (more flexible)    |

---

## Read vs. Write Operations

Not all operations are equal from a concurrency standpoint.

| Operation | Reads store? | Writes store? | Lock type needed |
|-----------|-------------|---------------|-----------------|
| GET       | Yes         | No            | Shared (read) lock |
| EXISTS    | Yes         | No            | Shared (read) lock |
| SET       | No          | Yes           | Exclusive (write) lock |
| DELETE    | Yes         | Yes           | Exclusive (write) lock |

A `std::shared_mutex` exploits this asymmetry:

- **Multiple threads can hold the shared lock simultaneously.** This means concurrent GET and EXISTS operations do not block each other — they execute in parallel.
- **Only one thread can hold the exclusive lock, and only when no shared locks are held.** SET and DELETE block all other readers and writers until they complete.

This is the **readers-writer lock** pattern. It is the correct model for a read-heavy workload, which is typical of a key-value store.

---

## Readers-Writer Lock in Action

```
Time ──────────────────────────────────────────────────────▶

Thread A:  [GET "name"  ─ shared lock ────────]
Thread B:         [GET "city" ─ shared lock ──────────]
Thread C:                  [SET "age" "22" ── waiting... ── exclusive lock ──]
Thread D:                        [GET "x" ─ waiting... ── shared lock ──]

Explanation:
  - Threads A and B read simultaneously (no conflict).
  - Thread C wants to write: it must wait for A and B to finish.
  - Thread D wants to read: it must also wait behind Thread C
    (to prevent writer starvation).
```

---

## Implementation Plan (Conceptual)

The `KeyValueStore` will hold a `std::shared_mutex` member. Every public method will acquire the appropriate lock before accessing the map.

```cpp
// Conceptual — not yet implemented

class KeyValueStore {
    std::unordered_map<std::string, std::string> store_;
    mutable std::shared_mutex mutex_;

public:
    // Read operations acquire shared lock
    std::optional<std::string> get(const std::string& key) const {
        std::shared_lock lock(mutex_);
        // ... read from store_ ...
    }

    // Write operations acquire exclusive lock
    void set(const std::string& key, const std::string& value) {
        std::unique_lock lock(mutex_);
        // ... write WAL record, then update store_ ...
    }
};
```

The RAII lock wrappers (`std::shared_lock`, `std::unique_lock`) ensure the lock is released when the method returns, even if an exception is thrown.

---

## WAL Concurrency

The WAL file is also a shared resource. Concurrent writes to the WAL from multiple threads must be serialized — two threads must not interleave their binary records.

The simplest correct approach: the WAL write and the in-memory update happen together under the exclusive lock. This means a write lock covers both the file write and the map update, guaranteeing they are atomic from the perspective of other threads.

More sophisticated approaches (separate WAL lock, write batching) are possible future improvements but are not planned at Stage 7.

---

## What Stage 7 Does Not Do

Stage 7 makes ForgeKV thread-safe under the readers-writer model. It does not:

- Introduce lock-free data structures
- Implement write batching or group commit
- Add transaction support (atomic multi-key operations)
- Change the HTTP server threading model

Those are potential future improvements beyond the current roadmap.

---

## Testing Concurrency

Concurrency bugs are notoriously difficult to reproduce in tests. Stage 13 will include:

- **Concurrent GET tests**: many threads reading simultaneously
- **Concurrent SET tests**: many threads writing simultaneously
- **Mixed read/write tests**: readers and writers running concurrently
- **Thread sanitizer (TSAN)**: compile-time instrumentation that detects data races at runtime

TSAN is the most reliable tool for catching data races and will be enabled in the test build configuration.

---

*Previous: [05-http-server.md](05-http-server.md)*  
*Next: [07-log-compaction.md](07-log-compaction.md)*
