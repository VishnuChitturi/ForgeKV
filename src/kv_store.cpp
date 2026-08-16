// =============================================================================
// ForgeKV — Stage 2: KeyValueStore implementation
// =============================================================================
//
// KeyValueStore no longer contains an unordered_map. All storage operations
// are delegated to the Storage implementation held in storage_.
//
// The unordered_map logic now lives in InMemoryStorage (src/in_memory_storage.cpp).
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"

#include <stdexcept>

namespace forgekv {

// -----------------------------------------------------------------------------
// Default constructor — creates an InMemoryStorage backing store.
// -----------------------------------------------------------------------------
KeyValueStore::KeyValueStore()
    : storage_(std::make_unique<InMemoryStorage>()) {}

// -----------------------------------------------------------------------------
// Dependency-injection constructor.
// -----------------------------------------------------------------------------
// Takes ownership of the provided Storage implementation.
// Throws if null is passed — a null storage would make the object unusable.
KeyValueStore::KeyValueStore(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)) {
    if (!storage_) {
        throw std::invalid_argument(
            "KeyValueStore: storage must not be null");
    }
}

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
void KeyValueStore::set(const std::string& key, const std::string& value) {
    storage_->set(key, value);
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    return storage_->get(key);
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
bool KeyValueStore::del(const std::string& key) {
    return storage_->del(key);
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
bool KeyValueStore::exists(const std::string& key) const {
    return storage_->exists(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
std::size_t KeyValueStore::size() const {
    return storage_->size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
bool KeyValueStore::empty() const {
    return storage_->empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
void KeyValueStore::clear() {
    storage_->clear();
}

} // namespace forgekv
