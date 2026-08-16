#pragma once

// =============================================================================
// ForgeKV — Stage 2: InMemoryStorage
// =============================================================================
//
// InMemoryStorage is the default concrete implementation of the Storage
// interface. It stores all key-value pairs in a std::unordered_map in RAM.
//
// This is exactly the backing store that KeyValueStore used directly in
// Stage 1 — now extracted behind the Storage interface so that KeyValueStore
// no longer depends on unordered_map.
//
// All operations retain their Stage 1 complexity:
//
//   set    — O(1) average (insert or overwrite via operator[])
//   get    — O(1) average (find, no modification)
//   del    — O(1) average (erase, returns count > 0)
//   exists — O(1) average (contains, C++20)
//   size   — O(1)
//   empty  — O(1)
//   clear  — O(n) — must visit every bucket
//
// Thread safety: NOT provided at this stage.
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

    // Not copyable and not movable through the base interface.
    // InMemoryStorage is always held via unique_ptr<Storage>; move semantics
    // on the concrete class are not needed.
    InMemoryStorage(const InMemoryStorage&)            = delete;
    InMemoryStorage& operator=(const InMemoryStorage&) = delete;
    InMemoryStorage(InMemoryStorage&&)                 = delete;
    InMemoryStorage& operator=(InMemoryStorage&&)      = delete;

    // -------------------------------------------------------------------------
    // Storage interface — overrides
    // -------------------------------------------------------------------------

    void set(const std::string& key, const std::string& value) override;

    [[nodiscard]] std::optional<std::string>
    get(const std::string& key) const override;

    bool del(const std::string& key) override;

    [[nodiscard]] bool exists(const std::string& key) const override;

    [[nodiscard]] std::size_t size() const override;

    [[nodiscard]] bool empty() const override;

    void clear() override;

private:
    // The backing data structure.
    // Ownership lives here, not in KeyValueStore.
    std::unordered_map<std::string, std::string> store_;
};

} // namespace forgekv
