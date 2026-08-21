#pragma once
// =============================================================================
// ForgeKV — Stage 7: KeyValueStore (Concurrency / Thread Safety)
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
// Stage 7: KeyValueStore is now thread-safe via a std::shared_mutex (mutex_).
//
//          Locking model:
//
//          READ operations (get, exists, size, empty):
//            Acquire std::shared_lock<std::shared_mutex> — multiple readers
//            may hold the lock simultaneously.
//
//          WRITE operations (set, del, clear):
//            Acquire std::unique_lock<std::shared_mutex> — exactly one writer
//            holds the lock; all readers and other writers are excluded.
//
//          The mutex_ is acquired for the FULL duration of each public
//          operation (both the WAL write and the storage mutation), so that
//          a write is atomic from the perspective of other threads. There is
//          no intermediate visible state where the WAL has been written but
//          the in-memory store has not yet been updated.
//
//          InMemoryStorage does NOT have its own mutex. All access to
//          storage_ is mediated through KeyValueStore which already holds the
//          appropriate lock before calling into it.
//
//          WAL is likewise not independently synchronized. The WAL's
//          ofstream is only written under the exclusive lock held by the
//          write operations, so no two concurrent writers can interleave
//          WAL records.
//
// Architecture (Stage 7):
//
//                   KeyValueStore  ← public thread-safe boundary
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
//   4. Store is ready for normal, thread-safe operation.
//
// Write ordering (unchanged from Stage 4):
//
//   KeyValueStore::set()   → [exclusive lock] → WAL::append_set()   → Storage::set()
//   KeyValueStore::del()   → [exclusive lock] → WAL::append_del()   → Storage::del()
//   KeyValueStore::clear() → [exclusive lock] → WAL::append_clear() → Storage::clear()
//
// Recovery ordering (does NOT write to WAL):
//
//   WAL::replay() → Recovery::run() → Storage::set/del/clear()
//   (Recovery runs in the constructor, before the store is exposed to threads.)
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
// Thread safety: THREAD-SAFE as of Stage 7 via std::shared_mutex.
// =============================================================================

#include "forgekv/recovery.h"
#include "forgekv/snapshot.h"
#include "forgekv/storage.h"
#include "forgekv/wal.h"

#include <memory>
#include <optional>
#include <shared_mutex>
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

    // Not copyable. Movable via custom move constructor.
    //
    // std::shared_mutex is non-movable, so the default-generated move
    // constructor is deleted by the compiler. We provide an explicit move
    // constructor that moves storage_ and wal_ and default-constructs a new
    // mutex_ for the moved-to object (the moved-from mutex state is irrelevant
    // because the moved-from store must not be used after a move).
    KeyValueStore(const KeyValueStore&)            = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    KeyValueStore(KeyValueStore&& other) noexcept;
    KeyValueStore& operator=(KeyValueStore&&) = delete;

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

    // -------------------------------------------------------------------------
    // Stage 8: Log Compaction
    // -------------------------------------------------------------------------

    // COMPACT: Rewrite the WAL to contain only the current live state.
    //
    // This eliminates all historical, redundant, and obsolete records from the
    // WAL, reducing both its file size and future recovery time.
    //
    // Compaction is a write operation that holds the exclusive lock for its
    // entire duration, ensuring that:
    //   - No concurrent SET/DELETE/CLEAR can modify the state mid-compaction.
    //   - No concurrent WAL append can interleave with the atomic file replace.
    //   - Readers cannot observe an inconsistent intermediate state.
    //
    // The resulting compacted WAL contains exactly one SET record per live key.
    // Keys that have been deleted are not written.
    // Records are written in lexicographic key order for determinism.
    //
    // After compaction, the store's in-memory state is unchanged and the WAL
    // stream points to the new, compacted file.  Subsequent SET/DELETE/CLEAR
    // operations append to the compacted WAL normally.
    //
    // Stage 9 note: compact() DELETES the snapshot file (if present) before
    // rewriting the WAL.  This prevents a stale snapshot from pointing to a
    // WAL offset that no longer exists in the newly written compacted WAL.
    //
    // Throws std::runtime_error if the compaction fails (e.g., temp file
    // cannot be created, rename fails, or WAL reopen fails).  On failure
    // before the rename, the original WAL is preserved intact.
    void compact();

    // -------------------------------------------------------------------------
    // Stage 9: Snapshots
    // -------------------------------------------------------------------------

    // SNAPSHOT: Create a full-state checkpoint and save it to disk atomically.
    //
    // A snapshot records:
    //   - All live key-value pairs at the moment of creation.
    //   - The WAL byte offset at the moment of creation.
    //
    // On subsequent startup, recovery will:
    //   1. Load the snapshot (restore in-memory state from saved pairs).
    //   2. Replay only WAL records written AFTER the snapshot (from the saved
    //      offset onward).
    //
    // This reduces recovery time: instead of replaying the full WAL history,
    // only the tail needs to be processed.
    //
    // Locking:
    //   snapshot() acquires the EXCLUSIVE lock for its entire duration.
    //   This guarantees that the captured state and the WAL offset represent
    //   the SAME logical point in time — no concurrent write can interleave
    //   between the state capture and the offset query.
    //
    // Atomicity:
    //   The snapshot file is written atomically via a temp-then-rename strategy.
    //   If writing fails before the rename, any existing snapshot is preserved.
    //
    // Returns true on success, false if snapshot creation failed.
    // Does not throw (all errors are logged/suppressed internally and the
    // store remains fully operational on snapshot failure).
    bool snapshot();

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

    // Reader/writer lock protecting storage_ and wal_ from concurrent access.
    //
    // Ownership model:
    //   - Read operations (get, exists, size, empty) acquire a shared_lock,
    //     allowing multiple concurrent readers.
    //   - Write operations (set, del, clear) acquire a unique_lock, granting
    //     exclusive access for the full duration of WAL write + storage mutation.
    //
    // mutex_ is declared mutable so that const read operations (get, exists,
    // size, empty) can acquire a shared_lock without violating const-ness.
    mutable std::shared_mutex mutex_;

    // Backing storage — owned exclusively. Never null after construction.
    std::unique_ptr<Storage> storage_;

    // Write-ahead log — owned exclusively. Never null after construction.
    std::unique_ptr<WAL> wal_;

    // Snapshot manager — manages the snapshot file at <wal_path>.snapshot.
    // Constructed lazily from wal_->path() after wal_ is initialised.
    SnapshotManager snapshot_manager_;
};

} // namespace forgekv
