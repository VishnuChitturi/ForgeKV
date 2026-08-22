#pragma once

// =============================================================================
// ForgeKV — Stage 10: InMemoryStorage (TTL support)
// =============================================================================
//
// InMemoryStorage is the concrete implementation of the Storage interface.
// It stores all key-value pairs (with optional expiration) in a
// std::unordered_map<std::string, StoreEntry> in RAM.
//
// Stage 10 changes:
//   - Backing store changed from unordered_map<string, string>
//     to unordered_map<string, StoreEntry>.
//   - All read operations (get, exists, get_all) respect expiry:
//     expired entries are treated as absent.
//   - set_with_expiry() stores an absolute expiration timestamp.
//   - set() stores a permanent entry (expires_at_us == 0), clearing any TTL.
//   - expire_keys() scans for and removes all expired entries.
//
// Expiry enforcement at read time:
//   get() and exists() check the current wall-clock time on every call
//   and treat expired entries as absent.  They do NOT remove expired entries
//   from the map (that would be a mutation in a logically-const path).
//   Actual removal happens via expire_keys() (called by background cleanup)
//   or lazily by KeyValueStore's write operations.
//
// Thread safety: NOT provided here. KeyValueStore owns the shared_mutex.
// =============================================================================

#include "forgekv/storage.h"

#include <string>
#include <optional>
#include <unordered_map>

namespace forgekv {

class InMemoryStorage final : public Storage {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    InMemoryStorage()  = default;
    ~InMemoryStorage() override = default;

    InMemoryStorage(const InMemoryStorage&)            = delete;
    InMemoryStorage& operator=(const InMemoryStorage&) = delete;
    InMemoryStorage(InMemoryStorage&&)                 = delete;
    InMemoryStorage& operator=(InMemoryStorage&&)      = delete;

    // -------------------------------------------------------------------------
    // Storage interface — overrides
    // -------------------------------------------------------------------------

    // Stage 2 interface (backward-compatible):

    // SET: upsert as permanent (clears any existing TTL).
    void set(const std::string& key, const std::string& value) override;

    // GET: returns value if key is present and not expired, else nullopt.
    [[nodiscard]] std::optional<std::string>
    get(const std::string& key) const override;

    // DEL: removes key (any TTL). Returns true if existed.
    bool del(const std::string& key) override;

    // EXISTS: returns true only if key is present AND not expired.
    [[nodiscard]] bool exists(const std::string& key) const override;

    [[nodiscard]] std::size_t size() const override;
    [[nodiscard]] bool empty() const override;
    void clear() override;

    // Returns all live (non-expired) (key, value) pairs.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>>
    get_all() const override;

    // Stage 10 TTL interface:

    // SET_WITH_EXPIRY: upsert with absolute expiration timestamp.
    void set_with_expiry(const std::string& key,
                         const std::string& value,
                         std::uint64_t      expires_at_us) override;

    // GET_ENTRY: return full StoreEntry (including expired ones).
    [[nodiscard]] std::optional<StoreEntry>
    get_entry(const std::string& key) const override;

    // GET_ALL_WITH_EXPIRY: return all live entries with their expiry data.
    [[nodiscard]] std::vector<std::pair<std::string, StoreEntry>>
    get_all_with_expiry(std::uint64_t now_us) const override;

    // EXPIRE_KEYS: remove all entries expired at now_us, return their keys.
    std::vector<std::string>
    expire_keys(std::uint64_t now_us) override;

private:
    // Helper: return current time in microseconds since Unix epoch.
    static std::uint64_t now_us() noexcept;

    // Backing store — StoreEntry carries both value and expiry metadata.
    std::unordered_map<std::string, StoreEntry> store_;
};

} // namespace forgekv
