#pragma once
// =============================================================================
// ForgeKV — Stage 5: KeyValueStore (Crash Recovery / WAL Replay)
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
//          The Stage 3 WAL used a human-readable text format.
// Stage 4: The WAL now uses a structured binary format with CRC32 checksums.
//          Keys and values are stored with explicit byte lengths, so any byte
//          sequence (including '|', '\n', '\r', spaces) is handled correctly.
//          The KeyValueStore API and write-ordering invariants are unchanged.
// Stage 5: On construction, KeyValueStore replays the existing WAL into
//          Storage, reconstructing the key-value state from before the last
//          shutdown.  All three constructors perform recovery automatically.
//          Recovery does NOT write new WAL records.  After recovery, normal
//          mutations continue appending to the existing WAL.
//
// Architecture (Stage 5):
//
//                   KeyValueStore
//                  /     |       \
//                 /      |        \
//                v       v         v
//            Storage  Recovery    WAL
//                |       |         |
//                v       v         v
//        InMemoryStorage  ←  forgekv.wal (binary)
//
// Startup sequence:
//
//   1. Construct Storage (empty).
//   2. Open WAL (append mode — existing content preserved).
//   3. Recovery::run() replays WAL records into Storage.
//   4. Store is ready for normal operation.
//
// Write ordering (unchanged from Stage 4):
//
//   KeyValueStore::set()   → WAL::append_set()   → Storage::set()
//   KeyValueStore::del()   → WAL::append_del()   → Storage::del()
//   KeyValueStore::clear() → WAL::append_clear() → Storage::clear()
//
// Recovery ordering (does NOT write to WAL):
//
//   WAL::replay() → Recovery::run() → Storage::set/del/clear()
//
// Recovery failure:
//
//   If the WAL contains a corrupted complete record (bad magic, version,
//   opcode, or checksum mismatch), or a truncated record that is NOT the
//   final entry, the constructor throws std::runtime_error.  The store
//   must not be used after a constructor throw.
//
//   A truncated FINAL record (common after a crash mid-write) is treated
//   as non-fatal: all prior complete records are replayed and the store
//   starts normally.
//
// New WAL file / empty WAL:
//
//   If the WAL file does not exist or is empty, recovery is a no-op.
//   The store starts with an empty Storage, as before Stage 5.
//
// Thread safety: NOT thread-safe at this stage. Stage 7 adds synchronization.
// =============================================================================

#include "forgekv/recovery.h"
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
    // "forgekv.wal" in the current working directory.  Performs WAL replay
    // into Storage before the store is ready for use.
    // Throws std::runtime_error if the WAL file cannot be opened or if
    // recovery encounters unrecoverable corruption.
    KeyValueStore();

    // Full dependency-injection constructor — accepts any Storage and WAL.
    // Takes ownership of both pointers.  Performs WAL replay into Storage.
    // Throws std::invalid_argument if storage or wal is null.
    // Throws std::runtime_error if recovery fails.
    explicit KeyValueStore(std::unique_ptr<Storage> storage,
                           std::unique_ptr<WAL>     wal);

    // Storage-only injection (convenience overload) — creates a default WAL
    // at "forgekv.wal". Kept for backward compatibility with Stage 2 tests
    // that inject only a Storage.  Performs WAL replay into Storage.
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
    // -------------------------------------------------------------------------
    // Recovery helper
    // -------------------------------------------------------------------------

    // Replay the WAL into storage_.  Called once from each constructor after
    // both storage_ and wal_ are initialised.
    //
    // On success (clean WAL, empty WAL, or truncated-final-record):
    //   returns normally.  storage_ contains the reconstructed state.
    //
    // On failure (corrupted record, mid-log truncation):
    //   throws std::runtime_error.  The constructor propagates the exception
    //   and the store must not be used.
    void recover();

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    // Backing storage — owned exclusively. Never null after construction.
    std::unique_ptr<Storage> storage_;

    // Write-ahead log — owned exclusively. Never null after construction.
    std::unique_ptr<WAL> wal_;
};

} // namespace forgekv
