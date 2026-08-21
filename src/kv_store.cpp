// =============================================================================
// ForgeKV — Stage 7: KeyValueStore implementation (Concurrency / Thread Safety)
// =============================================================================
//
// Stage 7 makes KeyValueStore thread-safe using a std::shared_mutex (mutex_).
//
// Locking strategy:
//
//   READ operations (get, exists, size, empty):
//     Acquire std::shared_lock<std::shared_mutex> — multiple readers may
//     proceed concurrently as long as no writer holds the lock.
//
//   WRITE operations (set, del, clear):
//     Acquire std::unique_lock<std::shared_mutex> — the caller gets exclusive
//     access; all concurrent readers and writers are blocked until the lock
//     is released.
//
// WAL + storage atomicity:
//
//   Each write operation acquires the exclusive lock FIRST, then writes to the
//   WAL, then updates in-memory storage, then releases the lock. Because the
//   lock is held for the full duration, no other thread can observe a state
//   where the WAL record exists but the in-memory store has not yet been
//   updated (or vice versa). Two concurrent writers cannot interleave their
//   WAL records.
//
// del() atomicity:
//
//   The check-then-act sequence (exists → WAL append → storage del) runs
//   entirely under the exclusive lock so it cannot race with a concurrent set
//   or another del on the same key.
//
// Recovery:
//
//   Recovery runs in the constructor, before the store object is returned to
//   the caller. No other thread can access the store during construction, so
//   no locking is needed inside recover().
//
// InMemoryStorage / WAL synchronization:
//
//   Neither InMemoryStorage nor WAL has its own mutex. All access to both is
//   serialized through KeyValueStore's mutex_. Adding a second mutex in either
//   component would create the potential for lock-order problems with no
//   benefit, because neither is ever accessed outside of KeyValueStore.
//
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/recovery.h"

#include <shared_mutex>

namespace forgekv {

// Default WAL path used when none is explicitly provided.
static constexpr const char* kDefaultWalPath = "forgekv.wal";

// -----------------------------------------------------------------------------
// Default constructor
// -----------------------------------------------------------------------------
// Creates an InMemoryStorage and opens the WAL at the default path.
// Performs crash recovery from the existing WAL file.
KeyValueStore::KeyValueStore()
    : storage_(std::make_unique<InMemoryStorage>()),
      wal_(std::make_unique<WAL>(kDefaultWalPath))
{
    recover();
}

// -----------------------------------------------------------------------------
// Full dependency-injection constructor
// -----------------------------------------------------------------------------
// Takes ownership of both the storage and the WAL.
// Performs crash recovery using the provided WAL.
// Throws std::invalid_argument if either pointer is null.
KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage,
                             std::unique_ptr<WAL>     wal)
    : storage_(std::move(storage)),
      wal_(std::move(wal))
{
    if (!storage_) {
        throw std::invalid_argument(
            "KeyValueStore: storage must not be null");
    }
    if (!wal_) {
        throw std::invalid_argument(
            "KeyValueStore: wal must not be null");
    }
    recover();
}

// -----------------------------------------------------------------------------
// Storage-only injection constructor (backward compatibility)
// -----------------------------------------------------------------------------
// Opens the WAL at the default path.
// Throws std::invalid_argument if storage is null.
KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)),
      wal_(std::make_unique<WAL>(kDefaultWalPath))
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
// std::shared_mutex is non-movable, so we provide an explicit move constructor
// that acquires an exclusive lock on the source, moves its resources into the
// new object, and default-constructs a fresh (unlocked) mutex_ for both.
//
// After a move, the source object's storage_ and wal_ are null. The source
// must not be used for any operation other than destruction.
KeyValueStore::KeyValueStore(KeyValueStore&& other) noexcept
{
    // Lock the source exclusively while we steal its resources.
    std::unique_lock lock(other.mutex_);
    storage_ = std::move(other.storage_);
    wal_     = std::move(other.wal_);
    // mutex_ for *this is default-constructed (unlocked) — no action needed.
}


// Replays the WAL into storage_.  Called once from each constructor.
//
// A truncated final record (common crash scenario) is non-fatal: all prior
// records are replayed and recovery succeeds.
//
// Any structural corruption throws std::runtime_error, which propagates out
// of the constructor.  The store must not be used after a failed construction.
void KeyValueStore::recover()
{
    Recovery recovery(*wal_, *storage_);
    (void)recovery.run(); // result (records_applied, incomplete_tail) not needed here;
}

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
// WAL-first: record the SET before applying it to storage.
// Exclusive lock: blocks all concurrent readers and writers.
void KeyValueStore::set(const std::string& key, const std::string& value)
{
    std::unique_lock lock(mutex_);
    wal_->append_set(key, value); // WAL write FIRST
    storage_->set(key, value);    // in-memory update SECOND
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
// Shared lock: multiple concurrent readers allowed.
std::optional<std::string> KeyValueStore::get(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->get(key);
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
// Only log a DEL when the key actually exists — the operation is a no-op
// otherwise, and a WAL record should represent a real state change.
//
// Exclusive lock: the entire check-then-act sequence (exists → WAL append →
// storage del) runs atomically. A concurrent set() on the same key cannot
// sneak in between the exists check and the WAL write.
bool KeyValueStore::del(const std::string& key)
{
    std::unique_lock lock(mutex_);
    if (!storage_->exists(key)) {
        return false; // key absent — nothing to log or remove
    }
    wal_->append_del(key); // WAL write FIRST
    storage_->del(key);    // in-memory removal SECOND
    return true;
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
// Shared lock: multiple concurrent readers allowed.
bool KeyValueStore::exists(const std::string& key) const
{
    std::shared_lock lock(mutex_);
    return storage_->exists(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
// Shared lock: multiple concurrent readers allowed.
std::size_t KeyValueStore::size() const
{
    std::shared_lock lock(mutex_);
    return storage_->size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
// Shared lock: multiple concurrent readers allowed.
bool KeyValueStore::empty() const
{
    std::shared_lock lock(mutex_);
    return storage_->empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
// WAL-first: record CLEAR before wiping storage.
// Exclusive lock: blocks all concurrent readers and writers.
void KeyValueStore::clear()
{
    std::unique_lock lock(mutex_);
    wal_->append_clear(); // WAL write FIRST
    storage_->clear();    // in-memory wipe SECOND
}

} // namespace forgekv
