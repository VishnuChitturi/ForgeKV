// =============================================================================
// ForgeKV — Stage 10: InMemoryStorage implementation (TTL support)
// =============================================================================
//
// Key design decisions:
//
// 1. Backing store: unordered_map<string, StoreEntry>
//    StoreEntry carries { string value; uint64_t expires_at_us; }.
//    expires_at_us == 0 → permanent.  >0 → expires at that wall-clock time.
//
// 2. size() and empty() count only LIVE (non-expired) entries.
//    The raw map may contain logically-expired entries that have not yet been
//    physically removed (background cleanup hasn't run, or no expire_keys()
//    call has happened yet).  size() filters them.
//
// 3. get() and exists() check the current time and treat expired entries as
//    absent WITHOUT removing them from the map (const-safe, no mutation).
//    Physical removal is done by expire_keys() under an exclusive lock.
//
// 4. expire_keys() scans all entries and removes those that are expired,
//    returning the removed keys so the caller (KeyValueStore) can write
//    WAL DEL records for durability.
//
// 5. set() clears any expiry (expires_at_us = 0) — a normal SET makes the
//    key permanent even if it previously had a TTL.
//
// 6. set_with_expiry() sets both value and expiry atomically.
// =============================================================================

#include "forgekv/in_memory_storage.h"

#include <chrono>

namespace forgekv {

// =============================================================================
// Time helper
// =============================================================================

// Returns the current UTC wall-clock time as microseconds since Unix epoch.
// Uses std::chrono::system_clock (wall clock, not monotonic).
// Wall-clock time is required so the value is meaningful across restarts.
std::uint64_t InMemoryStorage::now_us() noexcept {
    const auto now = std::chrono::system_clock::now();
    const auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch())
                         .count();
    return static_cast<std::uint64_t>(us);
}

// =============================================================================
// set — upsert as permanent (clears any existing TTL)
// =============================================================================
void InMemoryStorage::set(const std::string& key, const std::string& value) {
    store_[key] = StoreEntry(value, /*expires_at_us=*/0);
}

// =============================================================================
// set_with_expiry — upsert with absolute expiration timestamp
// =============================================================================
void InMemoryStorage::set_with_expiry(const std::string& key,
                                       const std::string& value,
                                       std::uint64_t      expires_at_us)
{
    // If expires_at_us == 0, treat as permanent (same as set()).
    store_[key] = StoreEntry(value, expires_at_us);
}

// =============================================================================
// get — return value if key is present and not expired, else nullopt
// =============================================================================
std::optional<std::string>
InMemoryStorage::get(const std::string& key) const {
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    // Check expiry without mutating.
    if (it->second.is_expired(now_us())) {
        return std::nullopt;
    }
    return it->second.value;
}

// =============================================================================
// get_entry — return full StoreEntry (including expired), or nullopt if absent
// =============================================================================
std::optional<StoreEntry>
InMemoryStorage::get_entry(const std::string& key) const {
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    return it->second; // intentionally returns expired entries
}

// =============================================================================
// del — remove key. Returns true if key existed (regardless of expiry state).
// =============================================================================
bool InMemoryStorage::del(const std::string& key) {
    return store_.erase(key) > 0;
}

// =============================================================================
// exists — returns true only if key is present AND not expired
// =============================================================================
bool InMemoryStorage::exists(const std::string& key) const {
    const auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    return !it->second.is_expired(now_us());
}

// =============================================================================
// size — count only live (non-expired) entries
// =============================================================================
std::size_t InMemoryStorage::size() const {
    const std::uint64_t t = now_us();
    std::size_t count = 0;
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired(t)) {
            ++count;
        }
    }
    return count;
}

// =============================================================================
// empty — true only if there are no live (non-expired) entries
// =============================================================================
bool InMemoryStorage::empty() const {
    const std::uint64_t t = now_us();
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired(t)) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// clear — remove all entries (permanent and expiring)
// =============================================================================
void InMemoryStorage::clear() {
    store_.clear();
}

// =============================================================================
// get_all — return (key, value) pairs for all live (non-expired) entries
// =============================================================================
std::vector<std::pair<std::string, std::string>>
InMemoryStorage::get_all() const {
    const std::uint64_t t = now_us();
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(store_.size());
    for (const auto& [key, entry] : store_) {
        if (!entry.is_expired(t)) {
            result.emplace_back(key, entry.value);
        }
    }
    return result;
}

// =============================================================================
// get_all_with_expiry — return all live entries with their full StoreEntry data
// =============================================================================
// now_us == 0: include all entries (no expiry filtering) — used for raw dumps.
// now_us  > 0: include only entries where !is_expired(now_us).
std::vector<std::pair<std::string, StoreEntry>>
InMemoryStorage::get_all_with_expiry(std::uint64_t now_us_val) const {
    std::vector<std::pair<std::string, StoreEntry>> result;
    result.reserve(store_.size());
    for (const auto& [key, entry] : store_) {
        if (now_us_val == 0 || !entry.is_expired(now_us_val)) {
            result.emplace_back(key, entry);
        }
    }
    return result;
}

// =============================================================================
// expire_keys — physically remove expired entries and return their keys
// =============================================================================
std::vector<std::string>
InMemoryStorage::expire_keys(std::uint64_t now_us_val) {
    std::vector<std::string> removed;
    for (auto it = store_.begin(); it != store_.end(); ) {
        if (it->second.is_expired(now_us_val)) {
            removed.push_back(it->first);
            it = store_.erase(it);
        } else {
            ++it;
        }
    }
    return removed;
}

} // namespace forgekv
