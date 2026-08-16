#pragma once

// =============================================================================
// ForgeKV — Stage 2: KeyValueStore (Storage-Abstraction refactor)
// =============================================================================
//
// KeyValueStore is the public-facing engine of ForgeKV. It maps string keys to
// string values and exposes the same API as Stage 1.
//
// Stage 1 vs Stage 2 — the key change:
//
//   Stage 1:
//     KeyValueStore owned an unordered_map directly.
//
//   Stage 2:
//     KeyValueStore owns a std::unique_ptr<Storage> and delegates all
//     storage operations through the interface. The concrete backing
//     store is hidden behind the pointer.
//
// Architecture:
//
//   KeyValueStore
//         │
//         ▼
//     Storage  (abstract interface — include/forgekv/storage.h)
//         │
//         ▼
//   InMemoryStorage  (default — wraps std::unordered_map)
//
// Default construction:
//   KeyValueStore store;
//   → automatically creates an InMemoryStorage as the backing store.
//   → existing Stage 1 code needs no changes.
//
// Dependency injection constructor:
//   KeyValueStore store(std::make_unique<SomeOtherStorage>(...));
//   → allows alternative implementations to be supplied.
//   → used in Stage 2 tests to verify the abstraction with a fake store.
//   → will be used in future stages (WAL-backed storage, etc.).
//
// Ownership model:
//   KeyValueStore owns the storage exclusively via unique_ptr.
//   It is not copyable (shared ownership would be non-obvious and rarely
//   correct). Move is allowed.
//
// Thread safety: NOT thread-safe at this stage. Stage 7 adds synchronization.
// =============================================================================

#include "forgekv/storage.h"

#include <memory>
#include <optional>
#include <string>

namespace forgekv {

class KeyValueStore {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Default constructor — creates an InMemoryStorage backing store.
    // This is the normal construction path; existing code requires no changes.
    KeyValueStore();

    // Dependency-injection constructor — accepts any Storage implementation.
    // Takes ownership of the storage pointer.
    // Precondition: storage must not be null.
    explicit KeyValueStore(std::unique_ptr<Storage> storage);

    ~KeyValueStore() = default;

    // Not copyable — copying a store would silently duplicate all data,
    // which is rarely intentional. Move is allowed.
    KeyValueStore(const KeyValueStore&)            = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    KeyValueStore(KeyValueStore&&)            = default;
    KeyValueStore& operator=(KeyValueStore&&) = default;

    // -------------------------------------------------------------------------
    // Core operations
    // -------------------------------------------------------------------------

    // SET: Store value under key. Overwrites if key already exists (upsert).
    void set(const std::string& key, const std::string& value);

    // GET: Return value for key, or nullopt if absent. No side effects.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    // DEL: Remove key and value. Returns true if key existed, false otherwise.
    bool del(const std::string& key);

    // EXISTS: Return true if key is present. No side effects.
    [[nodiscard]] bool exists(const std::string& key) const;

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    // Return the number of key-value pairs currently in the store.
    [[nodiscard]] std::size_t size() const;

    // Return true if the store contains no key-value pairs.
    [[nodiscard]] bool empty() const;

    // Remove all key-value pairs from the store.
    void clear();

private:
    // Backing storage — owned exclusively by this KeyValueStore instance.
    // Constructed at build time (default: InMemoryStorage) or injected.
    // Never null after construction.
    std::unique_ptr<Storage> storage_;
};

} // namespace forgekv
