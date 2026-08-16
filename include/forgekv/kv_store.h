#pragma once

// =============================================================================
// ForgeKV — Stage 1: In-Memory Key-Value Store
// =============================================================================
//
// KeyValueStore is the core abstraction of ForgeKV. It maps string keys to
// string values and supports four operations: set, get, del, exists.
//
// At Stage 1, all data lives in RAM. There is no persistence, no WAL, no
// networking, and no concurrency support. Those are added in later stages.
//
// Internal representation: std::unordered_map<std::string, std::string>
// The map is private. External code never interacts with it directly.
//
// Thread safety: NOT thread-safe at this stage. Stage 7 adds synchronization.
// =============================================================================

#include <optional>
#include <string>
#include <unordered_map>

namespace forgekv {

class KeyValueStore {
public:
    // -------------------------------------------------------------------------
    // Construction
    // -------------------------------------------------------------------------

    KeyValueStore()  = default;
    ~KeyValueStore() = default;

    // Not copyable — copying a store would silently duplicate all data,
    // which is rarely intentional. Move is allowed.
    KeyValueStore(const KeyValueStore&)            = delete;
    KeyValueStore& operator=(const KeyValueStore&) = delete;

    KeyValueStore(KeyValueStore&&)            = default;
    KeyValueStore& operator=(KeyValueStore&&) = default;

    // -------------------------------------------------------------------------
    // Core operations
    // -------------------------------------------------------------------------

    // SET: Store value under key. If the key already exists, its value is
    // overwritten (upsert semantics). Both empty strings are valid.
    void set(const std::string& key, const std::string& value);

    // GET: Return the value for key, or std::nullopt if the key is absent.
    // Does not modify the store.
    [[nodiscard]] std::optional<std::string> get(const std::string& key) const;

    // DEL: Remove key and its value. Safe to call on a key that does not
    // exist — it is a no-op in that case.
    // Returns true if the key existed and was removed, false otherwise.
    bool del(const std::string& key);

    // EXISTS: Return true if the key is present, false if it is not.
    // Does not modify the store.
    [[nodiscard]] bool exists(const std::string& key) const;

    // -------------------------------------------------------------------------
    // Utility
    // -------------------------------------------------------------------------

    // Return the number of key-value pairs currently in the store.
    [[nodiscard]] std::size_t size() const;

    // Return true if the store contains no key-value pairs.
    [[nodiscard]] bool empty() const;

    // Remove all key-value pairs from the store.
    void clear();

private:
    // The backing data structure. Private: no external code touches this.
    // std::unordered_map provides O(1) average for all four core operations.
    std::unordered_map<std::string, std::string> store_;
};

} // namespace forgekv
