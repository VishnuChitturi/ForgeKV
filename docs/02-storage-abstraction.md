# 02b — Storage Abstraction

> **Stage:** 2  
> **Status:** ✅ Complete. Stage 2 builds on the verified Stage 1 (`v0.1.0`) baseline.

---

## Overview

Stage 2 introduces a storage abstraction that decouples `KeyValueStore` from
`std::unordered_map`. The map implementation is moved behind a pure-virtual
`Storage` interface. `InMemoryStorage` is the default concrete implementation.

The public `KeyValueStore` API is unchanged — existing Stage 1 code compiles
and runs without modification.

---

## The Problem Stage 2 Solves

In Stage 1, `KeyValueStore` directly owned and operated a
`std::unordered_map`:

```
KeyValueStore
      │
      ▼
std::unordered_map<string, string>
```

This works for Stage 1, but creates a hard dependency on a single data
structure. When Stage 3 introduces a Write-Ahead Log (WAL), Stage 5 adds
crash recovery, or a future stage explores an on-disk B-tree, there would be
no clean place to insert the new storage backend — the map is baked into
`KeyValueStore` itself.

Stage 2 solves this by inserting an interface between them.

---

## Stage 2 Architecture

```
KeyValueStore
      │
      ▼
   Storage          ← abstract interface (include/forgekv/storage.h)
      │
      ▼
InMemoryStorage     ← concrete default (include/forgekv/in_memory_storage.h)
      │
      ▼
std::unordered_map<string, string>
```

`KeyValueStore` holds a `std::unique_ptr<Storage>`. It knows nothing about
`unordered_map`. `InMemoryStorage` is the default implementation created by
the `KeyValueStore` default constructor.

---

## The `Storage` Interface

```cpp
// include/forgekv/storage.h

namespace forgekv {

class Storage {
public:
    virtual ~Storage() = default;

    virtual void set(const std::string& key, const std::string& value) = 0;

    [[nodiscard]] virtual std::optional<std::string>
    get(const std::string& key) const = 0;

    virtual bool del(const std::string& key) = 0;

    [[nodiscard]] virtual bool exists(const std::string& key) const = 0;

    [[nodiscard]] virtual std::size_t size() const = 0;

    [[nodiscard]] virtual bool empty() const = 0;

    virtual void clear() = 0;
};

} // namespace forgekv
```

Key decisions:

- **Pure virtual** — `Storage` itself carries no data or logic.
- **Virtual destructor** — required for safe deletion through a base pointer
  (e.g. `std::unique_ptr<Storage>` calling the right destructor).
- **Const-correctness** — `get`, `exists`, `size`, `empty` are `const`.
  They do not mutate the store. `set`, `del`, `clear` are non-const.
- **No `unordered_map` in the interface** — the interface is a pure
  behavioural contract. The concrete backing structure is hidden.

---

## `InMemoryStorage`

```cpp
// include/forgekv/in_memory_storage.h

namespace forgekv {

class InMemoryStorage final : public Storage {
public:
    InMemoryStorage()  = default;
    ~InMemoryStorage() override = default;

    void set(const std::string& key, const std::string& value) override;
    std::optional<std::string> get(const std::string& key) const override;
    bool del(const std::string& key) override;
    bool exists(const std::string& key) const override;
    std::size_t size() const override;
    bool empty() const override;
    void clear() override;

private:
    std::unordered_map<std::string, std::string> store_;
};

} // namespace forgekv
```

`InMemoryStorage` is marked `final` — it is a leaf class. The `unordered_map`
lives here, privately. No other component touches it directly.

---

## `KeyValueStore` After Stage 2

```cpp
// include/forgekv/kv_store.h (simplified)

namespace forgekv {

class KeyValueStore {
public:
    // Default: creates InMemoryStorage internally.
    KeyValueStore();

    // Dependency injection: accepts any Storage implementation.
    explicit KeyValueStore(std::unique_ptr<Storage> storage);

    void set(const std::string& key, const std::string& value);
    std::optional<std::string> get(const std::string& key) const;
    bool del(const std::string& key);
    bool exists(const std::string& key) const;
    std::size_t size() const;
    bool empty() const;
    void clear();

private:
    std::unique_ptr<Storage> storage_;
};

} // namespace forgekv
```

Every method in `kv_store.cpp` delegates directly to `storage_`:

```cpp
void KeyValueStore::set(const std::string& key, const std::string& value) {
    storage_->set(key, value);
}
```

`KeyValueStore` no longer contains any data structure — it is purely a
façade over `Storage`.

---

## Ownership Model

`KeyValueStore` owns the `Storage` exclusively via `std::unique_ptr<Storage>`.
This means:

- The `Storage` object is destroyed when the `KeyValueStore` is destroyed.
- `KeyValueStore` cannot be copied (copying a store is non-obvious and rarely
  intended — Stage 1 already made this decision).
- Move is allowed.

---

## Dependency Injection

The injection constructor:

```cpp
explicit KeyValueStore(std::unique_ptr<Storage> storage);
```

allows any `Storage` implementation to be supplied at construction time.
This is used in the Stage 2 tests to verify the abstraction using a
`FakeStorage` that records which methods were called.

In future stages, this is how a WAL-backed storage will be plugged in:

```cpp
// (not yet implemented — deferred to Stage 3+)
auto wal_storage = std::make_unique<WalStorage>("path/to/wal");
KeyValueStore store(std::move(wal_storage));
```

`KeyValueStore` itself will not need to change.

---

## Stage 2 Tests

Tests are in `tests/test_kv_store.cpp`, appended after the Stage 1 suite.

| Test ID | What it tests |
|---------|---------------|
| S2-1    | InMemoryStorage set/get round-trip |
| S2-2    | InMemoryStorage update (overwrite) |
| S2-3    | InMemoryStorage get missing key → nullopt |
| S2-4    | InMemoryStorage del existing key |
| S2-5    | InMemoryStorage del missing key → false, no crash |
| S2-6    | InMemoryStorage exists (present and absent) |
| S2-7    | InMemoryStorage size and empty |
| S2-8    | InMemoryStorage clear |
| S2-9    | set/get through `Storage*` base pointer |
| S2-10   | del/exists through `Storage*` base pointer |
| S2-11   | size/empty/clear through `Storage*` base pointer |
| S2-12   | KeyValueStore default ctor still works (Stage 1 compatibility) |
| S2-13   | KeyValueStore update through abstraction |
| S2-14   | KeyValueStore clear through abstraction |
| S2-15   | KeyValueStore missing key returns nullopt through abstraction |
| S2-16   | KeyValueStore del missing key is safe through abstraction |
| S2-17   | FakeStorage DI: set() forwarded |
| S2-18   | FakeStorage DI: get() forwarded |
| S2-19   | FakeStorage DI: del() forwarded |
| S2-20   | FakeStorage DI: exists() forwarded |
| S2-21   | FakeStorage DI: size()/empty()/clear() forwarded |

All 15 Stage 1 tests still pass.

---

## What Stage 2 Does NOT Add

- No WAL, no persistence, no binary logs.
- No crash recovery.
- No HTTP server.
- No concurrency or locking.
- No snapshots, compaction, TTL, statistics, or benchmarking.

Those belong to later stages. Stage 2 is purely architectural — it
rearranges existing behaviour behind an interface without adding new behaviour.

---

## New Files

| File | Purpose |
|------|---------|
| `include/forgekv/storage.h` | Abstract `Storage` interface |
| `include/forgekv/in_memory_storage.h` | `InMemoryStorage` class declaration |
| `src/in_memory_storage.cpp` | `InMemoryStorage` implementation |

## Modified Files

| File | Change |
|------|--------|
| `include/forgekv/kv_store.h` | `unique_ptr<Storage>` replaces `unordered_map`; added DI constructor |
| `src/kv_store.cpp` | All methods delegate to `storage_`; includes `in_memory_storage.h` |
| `tests/test_kv_store.cpp` | 21 Stage 2 tests appended after Stage 1 tests |
| `CMakeLists.txt` | `in_memory_storage.cpp` added to `forgekv_core`; `forgekv_demo1` target added |
| `docs/02-in-memory-store.md` | Status updated to ✅ Complete |

---

*Previous: [02-in-memory-store.md](02-in-memory-store.md)*  
*Next: [03-write-ahead-log.md](03-write-ahead-log.md)*
