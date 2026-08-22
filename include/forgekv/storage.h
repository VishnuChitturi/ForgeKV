#pragma once

// =============================================================================
// ForgeKV — Stage 10: Storage Interface (TTL / Expiration)
// =============================================================================
//
// Storage is the abstract interface that decouples KeyValueStore from any
// particular backing data structure.
//
// Stage 10 adds TTL/expiration support:
//
//   - set_with_expiry: Store a key with an absolute expiration timestamp.
//   - get_entry:       Return the full StoreEntry (value + optional expiry).
//   - get_all_with_expiry: Return all live entries including expiry metadata.
//
// In-memory representation:
//   Each key maps to a StoreEntry which carries the string value and an
//   optional absolute expiration timestamp (microseconds since Unix epoch).
//
//   expires_at_us == 0 → permanent (no expiration).
//   expires_at_us  > 0 → expiring at that absolute wall-clock time.
//
// Key semantics:
//   - set(key, value)               → upsert as permanent (clears any TTL).
//   - set_with_expiry(key, v, ts)   → upsert with absolute expiration.
//   - get(key) / exists(key)        → expire-aware: returns nullopt / false
//                                     if the key is present but expired.
//   - del(key)                      → removes any entry, expiring or not.
//
// Thread safety: NOT provided at this level. KeyValueStore owns the mutex.
// =============================================================================

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace forgekv {

// =============================================================================
// StoreEntry — in-memory representation of a single key's data
// =============================================================================

struct StoreEntry {
    std::string   value;
    std::uint64_t expires_at_us{0}; // 0 = permanent; >0 = micros since epoch

    StoreEntry() = default;
    explicit StoreEntry(std::string v, std::uint64_t exp = 0)
        : value(std::move(v)), expires_at_us(exp) {}

    // Returns true if this entry has an expiration timestamp set.
    [[nodiscard]] bool has_expiry() const noexcept { return expires_at_us != 0; }

    // Returns true if the entry has expired relative to the provided
    // current time (in microseconds since epoch).
    [[nodiscard]] bool is_expired(std::uint64_t now_us) const noexcept {
        return has_expiry() && expires_at_us <= now_us;
    }
};

// =============================================================================
// Storage — abstract interface
// =============================================================================

class Storage {
public:
    // -------------------------------------------------------------------------
    // Virtual destructor — required for safe deletion through a base pointer.
    // -------------------------------------------------------------------------
    virtual ~Storage() = default;

    // -------------------------------------------------------------------------
    // Core operations (unchanged from Stage 2)
    // -------------------------------------------------------------------------

    // SET: Insert or overwrite the value for key as PERMANENT (no expiry).
    // Any existing TTL for this key is removed.
    // Upsert semantics.
    virtual void set(const std::string& key, const std::string& value) = 0;

    // GET: Return value for key, or nullopt if absent/expired.
    // Does NOT lazily remove expired entries (caller handles that separately).
    // No side effects.
    [[nodiscard]] virtual std::optional<std::string>
    get(const std::string& key) const = 0;

    // DEL: Remove key (including any TTL). Returns true if key existed.
    virtual bool del(const std::string& key) = 0;

    // EXISTS: Return true if key is present AND not expired.
    [[nodiscard]] virtual bool exists(const std::string& key) const = 0;

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    [[nodiscard]] virtual std::size_t size() const = 0;
    [[nodiscard]] virtual bool empty() const = 0;
    virtual void clear() = 0;

    // GET_ALL: Return (key, value) pairs for all live, non-expired keys.
    // Used by Stage 8 compaction. Expired keys are excluded.
    [[nodiscard]] virtual std::vector<std::pair<std::string, std::string>>
    get_all() const = 0;

    // -------------------------------------------------------------------------
    // Stage 10: TTL-aware operations
    // -------------------------------------------------------------------------

    // SET_WITH_EXPIRY: Insert or overwrite the value for key with an absolute
    // expiration timestamp. expires_at_us is microseconds since Unix epoch.
    // Upsert semantics. If expires_at_us == 0, behaves identically to set().
    virtual void set_with_expiry(const std::string& key,
                                 const std::string& value,
                                 std::uint64_t      expires_at_us) = 0;

    // GET_ENTRY: Return the full StoreEntry for key, or nullopt if absent.
    // Returns the entry even if it is expired — the caller decides what to do
    // with an expired entry.  Used internally by KeyValueStore for TTL queries.
    [[nodiscard]] virtual std::optional<StoreEntry>
    get_entry(const std::string& key) const = 0;

    // GET_ALL_WITH_EXPIRY: Return StoreEntry data for all keys whose
    // expires_at_us is greater than the provided `now_us` threshold
    // (i.e., still live) OR whose expires_at_us == 0 (permanent).
    // Used by snapshot() and compact() to obtain expiry-aware state.
    //
    // now_us: current time in microseconds since epoch.  Pass 0 to include
    //         all entries regardless of expiration (useful for raw dumps).
    [[nodiscard]] virtual std::vector<std::pair<std::string, StoreEntry>>
    get_all_with_expiry(std::uint64_t now_us) const = 0;

    // EXPIRE_KEYS: Remove all keys whose expires_at_us <= now_us.
    // Returns the list of keys that were removed so the caller can write
    // WAL DELETE records for durability.
    virtual std::vector<std::string>
    expire_keys(std::uint64_t now_us) = 0;

    // -------------------------------------------------------------------------
    // Non-copyable, non-movable base.
    // -------------------------------------------------------------------------
    Storage()                            = default;
    Storage(const Storage&)              = delete;
    Storage& operator=(const Storage&)  = delete;
    Storage(Storage&&)                   = delete;
    Storage& operator=(Storage&&)        = delete;
};

} // namespace forgekv
