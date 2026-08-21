// =============================================================================
// ForgeKV — Stage 5: KeyValueStore implementation (Crash Recovery / WAL Replay)
// =============================================================================
//
// Stage 5 adds WAL replay to all three constructors via the private recover()
// helper.  After both storage_ and wal_ are initialised, recover() runs
// Recovery::run() to apply WAL records to Storage, reconstructing the state
// that existed before the last shutdown.
//
// Recovery write ordering:
//
//   WAL::replay() → Recovery::run() → Storage::set/del/clear()
//
// Recovery bypasses KeyValueStore::set/del/clear entirely to prevent WAL
// record duplication.  The WAL is never truncated or rewritten during recovery.
//
// Every mutating operation after recovery follows the same write-ahead ordering
// as Stage 4:
//
//   1. Write the record to the WAL.
//   2. Only if the WAL write succeeds, apply the mutation to storage_.
//
// Read operations (get, exists, size, empty) bypass the WAL entirely and are
// unchanged from Stage 4.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/recovery.h"

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
// recover (private)
// -----------------------------------------------------------------------------
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
void KeyValueStore::set(const std::string& key, const std::string& value)
{
    wal_->append_set(key, value); // WAL write FIRST
    storage_->set(key, value);    // in-memory update SECOND
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
std::optional<std::string> KeyValueStore::get(const std::string& key) const
{
    return storage_->get(key);
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
// Only log a DEL when the key actually exists — the operation is a no-op
// otherwise, and a WAL record should represent a real state change.
bool KeyValueStore::del(const std::string& key)
{
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
bool KeyValueStore::exists(const std::string& key) const
{
    return storage_->exists(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
std::size_t KeyValueStore::size() const
{
    return storage_->size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
bool KeyValueStore::empty() const
{
    return storage_->empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
// WAL-first: record CLEAR before wiping storage.
void KeyValueStore::clear()
{
    wal_->append_clear(); // WAL write FIRST
    storage_->clear();    // in-memory wipe SECOND
}

} // namespace forgekv
