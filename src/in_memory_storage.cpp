// =============================================================================
// ForgeKV — Stage 2: InMemoryStorage implementation
// =============================================================================
//
// All logic here is the same as Stage 1's KeyValueStore implementation.
// The difference is that it now lives in its own translation unit, owned by
// InMemoryStorage, and accessed through the Storage interface.
//
// KeyValueStore no longer contains an unordered_map — it holds a
// std::unique_ptr<Storage> and delegates every call to this class.
// =============================================================================

#include "forgekv/in_memory_storage.h"

namespace forgekv {

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
// operator[] does insert-or-assign:
//   - key present  → value overwritten (upsert).
//   - key absent   → new entry created.
void InMemoryStorage::set(const std::string& key, const std::string& value) {
    store_[key] = value;
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
// find() is used instead of operator[] to avoid inserting a default entry
// for missing keys (operator[] would mutate the map on a const method, which
// the compiler would reject, and the side effect would be wrong either way).
std::optional<std::string> InMemoryStorage::get(const std::string& key) const {
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
// erase() returns the number of elements removed (0 or 1 for a unique-key map).
// Converting to bool: 1 → true (key existed), 0 → false (key was absent).
bool InMemoryStorage::del(const std::string& key) {
    return store_.erase(key) > 0;
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
// contains() is C++20. Pure read — no map mutation.
bool InMemoryStorage::exists(const std::string& key) const {
    return store_.contains(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
std::size_t InMemoryStorage::size() const {
    return store_.size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
bool InMemoryStorage::empty() const {
    return store_.empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
// Removes all entries; map left in a valid, empty state.
void InMemoryStorage::clear() {
    store_.clear();
}

} // namespace forgekv
