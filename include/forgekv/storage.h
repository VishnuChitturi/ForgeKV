#pragma once

// =============================================================================
// ForgeKV — Stage 2: Storage Interface
// =============================================================================
//
// Storage is the abstract interface that decouples KeyValueStore from any
// particular backing data structure.
//
// Stage 1 embedded std::unordered_map directly inside KeyValueStore.
// Stage 2 extracts the storage concern behind this interface, so that:
//
//   - KeyValueStore depends only on Storage, not on any concrete container.
//   - InMemoryStorage is the default implementation (wraps unordered_map).
//   - Future stages can introduce other implementations (WAL-backed, mmap'd,
//     etc.) without touching KeyValueStore at all.
//
// Interface contract:
//   - set:    Insert or overwrite. Always succeeds.
//   - get:    Return value for key, or nullopt if absent.
//   - del:    Remove key. Return true if key existed, false otherwise.
//   - exists: Return true if key is present. No side effects.
//   - size:   Return number of key-value pairs held.
//   - empty:  Return true if no pairs are held.
//   - clear:  Remove all pairs. Leave storage in a valid empty state.
//
// Const-correctness: get, exists, size, and empty are const — they do not
// mutate the storage. set, del, and clear are non-const — they mutate it.
//
// Thread safety: NOT provided at this stage. Stage 7 adds synchronization.
// =============================================================================

#include <optional>
#include <string>

namespace forgekv {

class Storage {
public:
    // -------------------------------------------------------------------------
    // Virtual destructor — required for safe deletion through a base pointer.
    // -------------------------------------------------------------------------
    virtual ~Storage() = default;

    // -------------------------------------------------------------------------
    // Core operations
    // -------------------------------------------------------------------------

    // SET: Insert or overwrite the value for key. Upsert semantics.
    virtual void set(const std::string& key, const std::string& value) = 0;

    // GET: Return value for key, or nullopt if absent. No side effects.
    [[nodiscard]] virtual std::optional<std::string>
    get(const std::string& key) const = 0;

    // DEL: Remove key. Returns true if key existed, false if it was absent.
    virtual bool del(const std::string& key) = 0;

    // EXISTS: Return true if key is present. No side effects.
    [[nodiscard]] virtual bool exists(const std::string& key) const = 0;

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    // SIZE: Return the number of key-value pairs currently held.
    [[nodiscard]] virtual std::size_t size() const = 0;

    // EMPTY: Return true if no key-value pairs are held.
    [[nodiscard]] virtual bool empty() const = 0;

    // CLEAR: Remove all key-value pairs. Storage is left valid and empty.
    virtual void clear() = 0;

    // -------------------------------------------------------------------------
    // Non-copyable, non-movable base.
    // -------------------------------------------------------------------------
    // Concrete implementations define their own copy/move semantics.
    // Copying or moving through the base-class interface is not meaningful.
    Storage()                            = default;
    Storage(const Storage&)              = delete;
    Storage& operator=(const Storage&)  = delete;
    Storage(Storage&&)                   = delete;
    Storage& operator=(Storage&&)        = delete;
};

} // namespace forgekv
