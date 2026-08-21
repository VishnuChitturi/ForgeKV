// =============================================================================
// ForgeKV — Stage 9: KeyValueStore implementation (Snapshots)
// =============================================================================
//
// Stage 9 adds two capabilities:
//
//   1. snapshot() — create a full-state binary checkpoint.
//
//      Under the exclusive lock, snapshot() captures all live key-value pairs
//      and the current WAL file size (byte offset).  It delegates to
//      SnapshotManager::save() which writes the snapshot atomically via a
//      temp-then-rename strategy.
//
//   2. Updated recover() — snapshot-aware startup recovery.
//
//      If a valid snapshot file exists at <wal_path>.snapshot:
//        a. Load the snapshot into in-memory storage.
//        b. Replay only WAL records at or after the snapshot's wal_offset.
//      If no snapshot exists (or if it is corrupt):
//        a. Fall back to full WAL replay from offset 0.
//      The logical state after either path is identical.
//
//   3. Updated compact() — deletes the snapshot before WAL rewrite.
//
//      Compaction rewrites the WAL from scratch (offset 0).  Any existing
//      snapshot's wal_offset would refer to the OLD WAL inode and is now
//      meaningless.  compact() removes the snapshot file before calling
//      wal_->rewrite(), so that subsequent recovery always uses WAL-only
//      replay (which is correct because the compacted WAL has the full state).
//
// All locking invariants from Stage 7 and Stage 8 are preserved unchanged.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/recovery.h"
#include "forgekv/snapshot.h"

#include <algorithm>
#include <iostream>
#include <shared_mutex>

namespace forgekv {

// Default WAL path used when none is explicitly provided.
static constexpr const char* kDefaultWalPath = "forgekv.wal";

// -----------------------------------------------------------------------------
// Default constructor
// -----------------------------------------------------------------------------
KeyValueStore::KeyValueStore()
    : storage_(std::make_unique<InMemoryStorage>()),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    recover();
}

// -----------------------------------------------------------------------------
// Full dependency-injection constructor
// -----------------------------------------------------------------------------
KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage,
                             std::unique_ptr<WAL>     wal)
    : storage_(std::move(storage)),
      wal_(std::move(wal)),
      snapshot_manager_("") // temporary — fixed below after null check
{
    if (!storage_) {
        throw std::invalid_argument(
            "KeyValueStore: storage must not be null");
    }
    if (!wal_) {
        throw std::invalid_argument(
            "KeyValueStore: wal must not be null");
    }
    // Reinitialise snapshot_manager_ now that we know wal_ is valid.
    snapshot_manager_ = SnapshotManager(wal_->path());
    recover();
}

// -----------------------------------------------------------------------------
// Storage-only injection constructor (backward compatibility)
// -----------------------------------------------------------------------------
KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)),
      wal_(std::make_unique<WAL>(kDefaultWalPath)),
      snapshot_manager_(kDefaultWalPath)
{
    if (!storage_) {
        throw std::invalid_argument(
            "KeyValueStore: storage must not be null");
    }
    recover();
}

// -----------------------------------------------------------------------------
// Move constructor
// -----------------------------------------------------------------------------
KeyValueStore::KeyValueStore(KeyValueStore&& other) noexcept
    : snapshot_manager_()  // default-initialised; overwritten below
{
    std::unique_lock lock(other.mutex_);
    storage_          = std::move(other.storage_);
    wal_              = std::move(other.wal_);
    snapshot_manager_ = std::move(other.snapshot_manager_);
}

// -----------------------------------------------------------------------------
// recover — snapshot-aware startup recovery
// -----------------------------------------------------------------------------
//
// Algorithm:
//
//   1. Ask the SnapshotManager to load the snapshot file.
//
//   2a. If a VALID snapshot exists:
//         - Apply all snapshot key-value pairs to storage_ directly.
//         - Call WAL::replay_from(wal_offset, callback) to replay only the
//           WAL tail written after the snapshot.
//
//   2b. If NO snapshot exists:
//         - Call WAL::replay() / Recovery::run() as before (full replay).
//
//   2c. If the snapshot is CORRUPT:
//         - Log a warning to stderr.
//         - Fall back to full WAL replay.
//         - The corrupt snapshot file is left on disk untouched (caller may
//           overwrite it with a new snapshot() call).
//
// Note: recovery runs in the constructor, before the store is exposed to
// threads.  No locking is needed here.
void KeyValueStore::recover()
{
    const auto snap_result = snapshot_manager_.load();

    if (snap_result.exists && !snap_result.corrupt) {
        // ----------------------------------------------------------------
        // Path A: valid snapshot found — load it, then replay WAL tail.
        // ----------------------------------------------------------------
        const SnapshotData& snap = snap_result.data;

        // Restore snapshot state into storage_.
        for (const auto& [key, value] : snap.records) {
            storage_->set(key, value);
        }

        // Replay WAL records written after the snapshot boundary.
        (void)wal_->replay_from(
            snap.wal_offset,
            [this](const WalRecord& rec) {
                switch (rec.opcode) {
                    case kOpSet:
                        storage_->set(rec.key, rec.value);
                        break;
                    case kOpDel:
                        storage_->del(rec.key);
                        break;
                    case kOpClear:
                        storage_->clear();
                        break;
                    default:
                        throw std::runtime_error(
                            "Recovery: unexpected opcode in WAL tail: "
                            + std::to_string(static_cast<int>(rec.opcode)));
                }
            });

    } else {
        // ----------------------------------------------------------------
        // Path B / C: no snapshot, or corrupt snapshot — full WAL replay.
        // ----------------------------------------------------------------
        if (snap_result.exists && snap_result.corrupt) {
            // Warn but do not abort. Fall back to WAL-only recovery.
            std::cerr << "[ForgeKV] WARNING: snapshot is corrupt and will be "
                         "ignored. Falling back to full WAL recovery. ("
                      << snap_result.error_msg << ")\n";
        }

        Recovery recovery(*wal_, *storage_);
        (void)recovery.run();
    }
}

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
void KeyValueStore::set(const std::string& key, const std::string& value)
{
    std::unique_lock lock(mutex_);
    wal_->append_set(key, value);
    storage_->set(key, value);
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
std::optional<std::string> KeyValueStore::get(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->get(key);
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
bool KeyValueStore::del(const std::string& key)
{
    std::unique_lock lock(mutex_);
    if (!storage_->exists(key)) {
        return false;
    }
    wal_->append_del(key);
    storage_->del(key);
    return true;
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
bool KeyValueStore::exists(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->exists(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
std::size_t KeyValueStore::size() const
{
    std::shared_lock lock(mutex_);
    return storage_->size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
bool KeyValueStore::empty() const
{
    std::shared_lock lock(mutex_);
    return storage_->empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
void KeyValueStore::clear()
{
    std::unique_lock lock(mutex_);
    wal_->append_clear();
    storage_->clear();
}

// -----------------------------------------------------------------------------
// compact
// -----------------------------------------------------------------------------
//
// Stage 9 addition: before calling wal_->rewrite(), remove any existing
// snapshot.  Compaction rewrites the WAL from offset 0.  A snapshot created
// before compaction stores a wal_offset into the OLD WAL's byte layout.
// After rewrite(), that offset is meaningless and would cause incorrect
// recovery (either seeking to the wrong position or past EOF).
//
// By deleting the snapshot first, we guarantee that:
//   - The compacted WAL is the sole source of truth for the next recovery.
//   - Recovery after compaction always uses full WAL replay (from offset 0),
//     which is correct because the compacted WAL already contains the full
//     current state.
//   - A new snapshot() call after compact() produces a snapshot that correctly
//     points into the new compacted WAL.
void KeyValueStore::compact()
{
    std::unique_lock lock(mutex_);

    // 1. Capture the complete current state.
    auto snap = storage_->get_all();

    // 2. Sort by key for deterministic WAL record order.
    std::sort(snap.begin(), snap.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 3. Delete any existing snapshot BEFORE rewriting the WAL.
    //    A failure here is non-fatal (we warn and continue).
    if (snapshot_manager_.exists()) {
        if (!snapshot_manager_.remove()) {
            std::cerr << "[ForgeKV] WARNING: compact() could not remove "
                         "existing snapshot at "
                      << snapshot_manager_.snapshot_path() << "\n";
        }
    }

    // 4. Delegate to WAL: write temp file, atomic rename, reopen stream.
    wal_->rewrite(snap);
}

// -----------------------------------------------------------------------------
// snapshot
// -----------------------------------------------------------------------------
//
// Create a full-state checkpoint under the exclusive lock.
//
// The exclusive lock ensures that no SET/DEL/CLEAR can run between the state
// capture (storage_->get_all()) and the WAL offset query (wal_->file_size()).
// Both steps see the same consistent logical state.
bool KeyValueStore::snapshot()
{
    try {
        std::unique_lock lock(mutex_);

        // 1. Capture the complete current state.
        auto records = storage_->get_all();

        // 2. Capture the WAL boundary (current end-of-file position).
        //    This is the offset past which future WAL records will be written.
        //    Recovery must replay only records at or after this offset.
        const std::uint64_t wal_offset = wal_->file_size();

        // 3. Write the snapshot atomically.
        snapshot_manager_.save(wal_offset, records);

        return true;

    } catch (const std::exception& e) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed: " << e.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[ForgeKV] ERROR: snapshot() failed (unknown exception)\n";
        return false;
    }
}

} // namespace forgekv
