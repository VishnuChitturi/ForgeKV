// =============================================================================
// ForgeKV — Stage 3: KeyValueStore implementation (WAL integration)
// =============================================================================
//
// Every mutating operation follows the write-ahead ordering:
//
//   1. Write the record to the WAL (wal_->append_*).
//   2. Only if the WAL write succeeds, apply the mutation to storage_.
//
// If the WAL write throws, the exception propagates to the caller and
// the in-memory state is left unchanged. The store remains consistent.
//
// Read operations (get, exists, size, empty) bypass the WAL entirely.
//
// del() special case:
//   A DEL record is only written to the WAL when the key actually exists.
//   If the key is absent, del() returns false immediately without touching
//   the WAL. This keeps the WAL semantically meaningful — every record in
//   the log represents an operation that changed state.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"

#include <stdexcept>

namespace forgekv {

// Default WAL path used when none is explicitly provided.
static constexpr const char* kDefaultWalPath = "forgekv.wal";

// -----------------------------------------------------------------------------
// Default constructor
// -----------------------------------------------------------------------------
// Creates an InMemoryStorage and opens the WAL at the default path.
KeyValueStore::KeyValueStore()
    : storage_(std::make_unique<InMemoryStorage>()),
      wal_(std::make_unique<WAL>(kDefaultWalPath))
{}

// -----------------------------------------------------------------------------
// Full dependency-injection constructor
// -----------------------------------------------------------------------------
// Takes ownership of both the storage and the WAL.
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
}

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
// WAL-first: record the SET before applying it to storage.
// If wal_ throws, the exception propagates and storage_ is not touched.
void KeyValueStore::set(const std::string& key, const std::string& value) {
    wal_->append_set(key, value);   // WAL write FIRST
    storage_->set(key, value);      // in-memory update SECOND
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    return storage_->get(key);
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
// Only log a DEL when the key actually exists — the operation is a no-op
// otherwise, and a WAL record should represent a real state change.
//
// WAL-first: record DEL before removing from storage.
// If wal_ throws, the exception propagates and storage_ is not touched.
bool KeyValueStore::del(const std::string& key) {
    if (!storage_->exists(key)) {
        return false;               // key absent — nothing to log or remove
    }
    wal_->append_del(key);          // WAL write FIRST
    storage_->del(key);             // in-memory removal SECOND
    return true;
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
bool KeyValueStore::exists(const std::string& key) const {
    return storage_->exists(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
std::size_t KeyValueStore::size() const {
    return storage_->size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
// Read-only. No WAL interaction.
bool KeyValueStore::empty() const {
    return storage_->empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
// WAL-first: record CLEAR before wiping storage.
// If wal_ throws, the exception propagates and storage_ is not touched.
void KeyValueStore::clear() {
    wal_->append_clear();   // WAL write FIRST
    storage_->clear();      // in-memory wipe SECOND
}

} // namespace forgekv
