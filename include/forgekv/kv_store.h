#pragma once

// =============================================================================
// ForgeKV — Stage 3: KeyValueStore (WAL integration)
// =============================================================================
//
// KeyValueStore is the public-facing engine of ForgeKV. It maps string keys to
// string values and exposes the same API as Stage 1.
//
// Stage 1: KeyValueStore owned an unordered_map directly.
// Stage 2: KeyValueStore owns a std::unique_ptr<Storage> and delegates all
//          storage operations through the interface.
// Stage 3: KeyValueStore also owns a std::unique_ptr<WAL>. Every mutating
//          operation writes a WAL record BEFORE touching in-memory storage.
//
// Architecture:
//
//                   KeyValueStore
//                  /             \
//                 /               \
//                v                 v
//            Storage             WAL
//                |                 |
//                v                 v
//        InMemoryStorage      forgekv.wal (text)
//                |
//                v
//          unordered_map
//
// Write ordering (Stage 3 invariant):
//
//   KeyValueStore::set()   → WAL::append_set()   → Storage::set()
//   KeyValueStore::del()   → WAL::append_del()   → Storage::del()
//   KeyValueStore::clear() → WAL::append_clear() → Storage::clear()
//
//   If a WAL write throws, the in-memory state is NOT changed.
//   The exception propagates to the caller.
//
// Read operations (get, exists, size, empty) do NOT write to the WAL.
//
// Default construction:
//   KeyValueStore store;
//   → creates InMemoryStorage and opens WAL at "forgekv.wal".
//   → existing Stage 1 and Stage 2 code needs no changes.
//
// Full dependency-injection constructor:
//   KeyValueStore store(std::move(storage), std::move(wal));
//   → accepts any Storage and any WAL.
//   → used in Stage 3 tests with a test-specific WAL path.
//
// Ownership model:
//   KeyValueStore owns both storage_ and wal_ exclusively via unique_ptr.
//   It is not copyable. Move is allowed.
//
// Thread safety: NOT thread-safe at this stage. Stage 7 adds synchronization.
// =============================================================================

#include "forgekv/storage.h"
#include "forgekv/wal.h"

#include <memory>
#include <optional>
#include <string>

namespace forgekv {

class KeyValueStore {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    // Default constructor — creates InMemoryStorage and opens WAL at
    // "forgekv.wal" in the current working directory.
    // Throws std::runtime_error if the WAL file cannot be opened.
    KeyValueStore();

    // Full dependency-injection constructor — accepts any Storage and WAL.
    // Takes ownership of both pointers.
    // Throws std::invalid_argument if storage or wal is null.
    explicit KeyValueStore(std::unique_ptr<Storage> storage,
                           std::unique_ptr<WAL>     wal);

    // Storage-only injection (convenience overload) — creates a default WAL
    // at "forgekv.wal". Kept for backward compatibility with Stage 2 tests
    // that inject only a Storage. The WAL is still opened and will be written
    // to on every mutation.
    explicit KeyValueStore(std::unique_ptr<Storage> storage);

    ~KeyValueStore() = default;

    // Not copyable. Move is allowed.
    KeyValueStore(const KeyValueStore&)            = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    KeyValueStore(KeyValueStore&&)            = default;
    KeyValueStore& operator=(KeyValueStore&&) = default;

    // -------------------------------------------------------------------------
    // Core operations
    // -------------------------------------------------------------------------

    // SET: WAL append_set, then Storage::set. Upsert semantics.
    // Throws std::runtime_error if the WAL write fails (in-memory unchanged).
    void set(const std::string& key, const std::string& value);

    // GET: Storage::get only. No WAL interaction.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    // DEL: If key exists: WAL append_del, then Storage::del. Returns true.
    //      If key does not exist: returns false. No WAL record is written.
    // Throws std::runtime_error if the WAL write fails (in-memory unchanged).
    bool del(const std::string& key);

    // EXISTS: Storage::exists only. No WAL interaction.
    [[nodiscard]] bool exists(const std::string& key) const;

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    // SIZE: Storage::size only. No WAL interaction.
    [[nodiscard]] std::size_t size() const;

    // EMPTY: Storage::empty only. No WAL interaction.
    [[nodiscard]] bool empty() const;

    // CLEAR: WAL append_clear, then Storage::clear.
    // Throws std::runtime_error if the WAL write fails (in-memory unchanged).
    void clear();

private:
    // Backing storage — owned exclusively. Never null after construction.
    std::unique_ptr<Storage> storage_;

    // Write-ahead log — owned exclusively. Never null after construction.
    std::unique_ptr<WAL> wal_;
};

} // namespace forgekv
