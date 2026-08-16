# 02 — In-Memory Store

> **Stage:** 1  
> **Status:** ✅ Complete. Stage 1 is tagged `v0.1.0` and fully verified.

---

## Overview

Stage 1 introduces the core of ForgeKV: a `KeyValueStore` class that stores string keys mapped to string values, entirely in RAM.

This is the foundation. Every stage that follows — persistence, recovery, HTTP, concurrency — is built on top of this abstraction. Getting it right matters.

---

## The Data Structure

The backing data structure is `std::unordered_map<std::string, std::string>`.

```cpp
// Conceptual representation — not yet implemented
std::unordered_map<std::string, std::string> store;

store["name"] = "Vishnu";
store["age"]  = "21";
store["city"] = "Bengaluru";
```

### Why `unordered_map`?

`std::unordered_map` is a hash table. It provides:

| Operation | Average Complexity | Worst Case |
|-----------|--------------------|------------|
| `GET`     | O(1)               | O(n)       |
| `SET`     | O(1)               | O(n)       |
| `DELETE`  | O(1)               | O(n)       |
| `EXISTS`  | O(1)               | O(n)       |

Worst case is O(n) only in pathological hash collision scenarios. Under normal usage with good keys, all operations are effectively constant time.

This makes it the right starting point: simple to use, correct semantics, and fast in practice.

### Why strings for both keys and values?

Keeping both sides as `std::string` is the simplest valid representation. It supports arbitrary keys and arbitrary values (including serialized JSON, numbers, or binary data encoded as strings). The type can always be narrowed later; starting broad keeps Stage 1 clean.

---

## The Four Operations

### SET

Stores a value under a key. If the key already exists, its value is overwritten.

```
SET "name" "Vishnu"
SET "age"  "21"
SET "name" "Vishnu Kumar"   ← overwrites previous value
```

Semantics: upsert (insert or update).

### GET

Retrieves the value for a given key. Returns the value if the key exists, or indicates absence if it does not.

```
GET "name"   → "Vishnu Kumar"
GET "city"   → (not found)
```

GET does not modify the store.

### DELETE

Removes a key and its associated value from the store. Has no effect if the key does not exist.

```
DELETE "age"
GET "age"    → (not found)
```

### EXISTS

Returns a boolean indicating whether a key is currently present in the store.

```
EXISTS "name"   → true
EXISTS "age"    → false   (after DELETE above)
```

EXISTS is a read operation. It does not modify the store.

---

## Operation Table

| Operation | Reads store? | Writes store? | Return value         |
|-----------|-------------|---------------|----------------------|
| SET       | No          | Yes           | None (or success/failure) |
| GET       | Yes         | No            | Value or "not found" |
| DELETE    | Yes         | Yes           | None (or success/failure) |
| EXISTS    | Yes         | No            | Boolean              |

This distinction matters later. In Stage 7 (Concurrency), read operations can be served simultaneously by multiple threads, but write operations require exclusive access. The read/write characterization of each operation is established here.

---

## Why This Stage Matters

Stage 1 may look trivial — it is just a wrapper around a standard library container. But it establishes several things:

1. **The abstraction boundary.** `KeyValueStore` is the entity the rest of the system talks to. Nothing outside it should know what data structure is used internally.

2. **The operation contract.** SET, GET, DELETE, EXISTS have defined semantics here. Later stages (WAL, HTTP, TTL) must preserve these semantics exactly.

3. **The testable baseline.** Stage 13 will test every component. Stage 1 must be independently testable before persistence or networking are introduced.

4. **The performance baseline.** Stage 12 will benchmark ForgeKV. The in-memory store performance is the ceiling — no later stage should be significantly slower for the same operation without justification.

---

## Limitations at This Stage

| Limitation           | Addressed In   |
|----------------------|----------------|
| Data lost on exit    | Stage 3 (WAL)  |
| Not thread-safe      | Stage 7 (Concurrency) |
| No network access    | Stage 6 (HTTP) |
| No key expiration    | Stage 10 (TTL) |
| No statistics        | Stage 11       |

These are not bugs in Stage 1 — they are deliberately deferred to keep the initial implementation clean and focused.

---

## In-Memory Store Diagram

```
┌──────────────────────────────────────────────┐
│               KeyValueStore                  │
│                                              │
│  ┌────────────────────────────────────────┐  │
│  │  std::unordered_map<string, string>    │  │
│  │                                        │  │
│  │  "name" ──▶ "Vishnu"                  │  │
│  │  "age"  ──▶ "21"                      │  │
│  │  "city" ──▶ "Bengaluru"               │  │
│  └────────────────────────────────────────┘  │
│                                              │
│  set(key, value)                             │
│  get(key) → optional<string>                │
│  del(key)                                    │
│  exists(key) → bool                          │
└──────────────────────────────────────────────┘
```

---

## What Stage 2 Adds

Stage 2 (Storage Abstraction) wraps this concrete `unordered_map` implementation behind an interface. `KeyValueStore` now depends on the `Storage` interface, not on `std::unordered_map` directly. `InMemoryStorage` provides the default concrete implementation. This makes it possible to swap the backing store in future stages without touching `KeyValueStore`'s public API or any code that depends on it.

See [02-storage-abstraction.md](02-storage-abstraction.md) for the full Stage 2 design.

---

*Previous: [01-project-overview.md](01-project-overview.md)*  
*Next: [02-storage-abstraction.md](02-storage-abstraction.md)*
