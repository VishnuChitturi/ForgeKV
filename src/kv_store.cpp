// =============================================================================
// ForgeKV — Stage 1: KeyValueStore implementation
// =============================================================================

#include "forgekv/kv_store.h"

namespace forgekv {

// -----------------------------------------------------------------------------
// set
// -----------------------------------------------------------------------------
// operator[] on unordered_map does an insert-or-assign in one step:
//   - If key exists:  the existing entry's value is overwritten.
//   - If key absent:  a new entry is created.
// This is exactly the upsert (insert-or-update) semantics we want for SET.
void KeyValueStore::set(const std::string& key, const std::string& value) {
    store_[key] = value;
}

// -----------------------------------------------------------------------------
// get
// -----------------------------------------------------------------------------
// We use find() rather than operator[] because operator[] would insert a
// default-constructed entry for missing keys — a side effect we do not want
// on a read operation.
//
// find() returns an iterator:
//   - If the key exists:  iterator points to the {key, value} pair.
//   - If the key is absent: iterator equals store_.end().
//
// We return std::optional<std::string> so the caller is forced to handle
// the "not found" case explicitly. There is no sentinel value (like "" or "-1")
// that could be confused with a legitimate stored value.
std::optional<std::string> KeyValueStore::get(const std::string& key) const {
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;   // key not present
    }
    return it->second;         // return a copy of the value
}

// -----------------------------------------------------------------------------
// del
// -----------------------------------------------------------------------------
// erase() removes the entry with the given key if it exists.
//   - Returns 1 (number of elements removed) if the key was present.
//   - Returns 0 if the key was absent (no error, no-op).
//
// We convert the count to bool: 1 → true (key existed and was removed),
//                                0 → false (key was not present).
bool KeyValueStore::del(const std::string& key) {
    return store_.erase(key) > 0;
}

// -----------------------------------------------------------------------------
// exists
// -----------------------------------------------------------------------------
// contains() is C++20. It returns true if the key is in the map, false if not.
// It is a pure read — no modification to the map.
//
// (Pre-C++20 equivalent: store_.count(key) > 0)
bool KeyValueStore::exists(const std::string& key) const {
    return store_.contains(key);
}

// -----------------------------------------------------------------------------
// size
// -----------------------------------------------------------------------------
// Returns the number of key-value pairs currently held.
// Delegated directly to the map's own size().
std::size_t KeyValueStore::size() const {
    return store_.size();
}

// -----------------------------------------------------------------------------
// empty
// -----------------------------------------------------------------------------
// Returns true when there are no entries. Equivalent to size() == 0,
// but potentially O(1) by contract (it is O(1) for unordered_map).
bool KeyValueStore::empty() const {
    return store_.empty();
}

// -----------------------------------------------------------------------------
// clear
// -----------------------------------------------------------------------------
// Removes all entries. The map is left in a valid, empty state.
void KeyValueStore::clear() {
    store_.clear();
}

} // namespace forgekv
