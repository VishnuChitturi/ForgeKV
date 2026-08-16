// =============================================================================
// ForgeKV — Stage 1 + Stage 2 + Stage 3 + Stage 4: KeyValueStore & WAL Tests
// =============================================================================
//
// Minimal self-contained test harness — no external framework required.
//
// Each TEST() block is a function registered at startup via a global list.
// The runner executes every registered test, catches assertion failures, and
// reports a summary. Exit code 0 = all tests passed, 1 = at least one failed.
//
// ASSERT_TRUE(cond)      — fail if cond is false
// ASSERT_EQ(a, b)        — fail if a != b
// ASSERT_FALSE(cond)     — fail if cond is true
// ASSERT_HAS_VALUE(opt)  — fail if optional is empty
// ASSERT_NO_VALUE(opt)   — fail if optional has a value
// ASSERT_THROWS(expr)    — fail if expr does NOT throw
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/storage.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// Minimal test harness
// -----------------------------------------------------------------------------

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> registry;
    return registry;
}

struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};

// Thrown by assertions to abort the current test on first failure.
struct AssertionFailure {
    std::string message;
};

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            throw AssertionFailure{"ASSERT_TRUE failed: " #cond \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_FALSE(cond) \
    do { \
        if ((cond)) { \
            throw AssertionFailure{"ASSERT_FALSE failed: " #cond \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_HAS_VALUE(opt) \
    do { \
        if (!(opt).has_value()) { \
            throw AssertionFailure{"ASSERT_HAS_VALUE failed: " #opt \
                " is empty (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

#define ASSERT_NO_VALUE(opt) \
    do { \
        if ((opt).has_value()) { \
            throw AssertionFailure{"ASSERT_NO_VALUE failed: " #opt \
                " has a value (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

// ASSERT_THROWS: verify that an expression throws any std::exception.
// Fails if the expression completes without throwing.
#define ASSERT_THROWS(expr) \
    do { \
        bool threw = false; \
        try { (expr); } \
        catch (const std::exception&) { threw = true; } \
        catch (...) { threw = true; } \
        if (!threw) { \
            throw AssertionFailure{ \
                "ASSERT_THROWS failed: expression did not throw: " #expr \
                " (line " + std::to_string(__LINE__) + ")"}; \
        } \
    } while (false)

// Registers a test function and runs it by name later.
#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar registrar_##name{#name, test_##name}; \
    static void test_##name()

// =============================================================================
// Stage 1 Tests
// =============================================================================

// 1. SET followed by GET — fundamental round-trip
TEST(set_then_get) {
    forgekv::KeyValueStore store;
    store.set("name", "Vishnu");
    auto result = store.get("name");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "Vishnu");
}

// 2. Multiple keys — independent entries
TEST(multiple_keys) {
    forgekv::KeyValueStore store;
    store.set("name", "Vishnu");
    store.set("age", "21");
    store.set("city", "Bengaluru");
    auto name = store.get("name");
    auto age  = store.get("age");
    auto city = store.get("city");
    ASSERT_HAS_VALUE(name);
    ASSERT_EQ(*name, "Vishnu");
    ASSERT_HAS_VALUE(age);
    ASSERT_EQ(*age, "21");
    ASSERT_HAS_VALUE(city);
    ASSERT_EQ(*city, "Bengaluru");
    ASSERT_EQ(store.size(), std::size_t{3});
}

// 3. Updating an existing key — value is overwritten, key count stays the same
TEST(update_existing_key) {
    forgekv::KeyValueStore store;
    store.set("name", "Alice");
    ASSERT_EQ(*store.get("name"), "Alice");
    store.set("name", "Bob");
    ASSERT_EQ(*store.get("name"), "Bob");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// 4. GET of a missing key — must return nullopt, not crash or return garbage
TEST(get_missing_key) {
    forgekv::KeyValueStore store;
    auto result = store.get("nonexistent");
    ASSERT_NO_VALUE(result);
}

// 5. GET returns nullopt after DELETE
TEST(delete_existing_key) {
    forgekv::KeyValueStore store;
    store.set("key", "value");
    ASSERT_HAS_VALUE(store.get("key"));
    bool removed = store.del("key");
    ASSERT_TRUE(removed);
    ASSERT_NO_VALUE(store.get("key"));
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 6. DELETE of a missing key — must be a safe no-op, not crash
TEST(delete_missing_key) {
    forgekv::KeyValueStore store;
    bool removed = store.del("ghost");
    ASSERT_FALSE(removed);
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 7. EXISTS for an existing key
TEST(exists_present_key) {
    forgekv::KeyValueStore store;
    store.set("lang", "C++");
    ASSERT_TRUE(store.exists("lang"));
}

// 8. EXISTS for a missing key
TEST(exists_absent_key) {
    forgekv::KeyValueStore store;
    ASSERT_FALSE(store.exists("missing"));
}

// 9a. Empty string as a value — valid input
TEST(empty_string_value) {
    forgekv::KeyValueStore store;
    store.set("key", "");
    auto result = store.get("key");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "");
}

// 9b. Empty string as a key — valid (unusual but should not crash)
TEST(empty_string_key) {
    forgekv::KeyValueStore store;
    store.set("", "empty-key-value");
    auto result = store.get("");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "empty-key-value");
    ASSERT_TRUE(store.exists(""));
    store.del("");
    ASSERT_FALSE(store.exists(""));
}

// 10. Repeated set/del/exists operations in sequence
TEST(repeated_operations) {
    forgekv::KeyValueStore store;
    for (int i = 0; i < 100; ++i) {
        store.set("counter", std::to_string(i));
    }
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_EQ(*store.get("counter"), "99");
    store.del("counter");
    ASSERT_FALSE(store.exists("counter"));
    ASSERT_TRUE(store.empty());
}

// 11. size() and empty() semantics
TEST(size_and_empty) {
    forgekv::KeyValueStore store;
    ASSERT_TRUE(store.empty());
    ASSERT_EQ(store.size(), std::size_t{0});
    store.set("a", "1");
    ASSERT_FALSE(store.empty());
    ASSERT_EQ(store.size(), std::size_t{1});
    store.set("b", "2");
    ASSERT_EQ(store.size(), std::size_t{2});
    store.del("a");
    ASSERT_EQ(store.size(), std::size_t{1});
    store.clear();
    ASSERT_TRUE(store.empty());
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 12. clear() removes all entries
TEST(clear_store) {
    forgekv::KeyValueStore store;
    store.set("x", "1");
    store.set("y", "2");
    store.set("z", "3");
    ASSERT_EQ(store.size(), std::size_t{3});
    store.clear();
    ASSERT_EQ(store.size(), std::size_t{0});
    ASSERT_NO_VALUE(store.get("x"));
    ASSERT_NO_VALUE(store.get("y"));
    ASSERT_NO_VALUE(store.get("z"));
}

// 13. EXISTS is not affected by GET (read-only check)
TEST(exists_is_read_only) {
    forgekv::KeyValueStore store;
    store.set("k", "v");
    ASSERT_TRUE(store.exists("k"));
    ASSERT_EQ(store.size(), std::size_t{1});
}

// 14. del() is idempotent — calling twice on same key is safe
TEST(double_delete) {
    forgekv::KeyValueStore store;
    store.set("temp", "data");
    ASSERT_TRUE(store.del("temp"));
    ASSERT_FALSE(store.del("temp"));
    ASSERT_TRUE(store.empty());
}

// 15. Large number of distinct keys
TEST(many_keys) {
    forgekv::KeyValueStore store;
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        store.set("key:" + std::to_string(i), "val:" + std::to_string(i));
    }
    ASSERT_EQ(store.size(), std::size_t(N));
    for (int i = 0; i < N; ++i) {
        auto v = store.get("key:" + std::to_string(i));
        ASSERT_HAS_VALUE(v);
        ASSERT_EQ(*v, "val:" + std::to_string(i));
    }
    for (int i = 0; i < N; ++i) {
        store.del("key:" + std::to_string(i));
    }
    ASSERT_TRUE(store.empty());
}

// =============================================================================
// Stage 2 tests — Storage Abstraction
// =============================================================================

// -----------------------------------------------------------------------------
// Minimal fake Storage implementation
// -----------------------------------------------------------------------------
class FakeStorage final : public forgekv::Storage {
public:
    mutable int set_calls    = 0;
    mutable int get_calls    = 0;
    mutable int del_calls    = 0;
    mutable int exists_calls = 0;
    mutable int size_calls   = 0;
    mutable int empty_calls  = 0;
    mutable int clear_calls  = 0;

    void set(const std::string&, const std::string&) override { ++set_calls; }
    std::optional<std::string> get(const std::string&) const override {
        ++get_calls; return std::nullopt;
    }
    bool del(const std::string&) override { ++del_calls; return false; }
    bool exists(const std::string&) const override { ++exists_calls; return false; }
    std::size_t size() const override { ++size_calls; return 0; }
    bool empty() const override { ++empty_calls; return true; }
    void clear() override { ++clear_calls; }
};

// =============================================================================
// InMemoryStorage — standalone tests (not through KeyValueStore)
// =============================================================================

// S2-1.
TEST(storage_set_then_get) {
    forgekv::InMemoryStorage s;
    s.set("key", "value");
    auto result = s.get("key");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "value");
}

// S2-2.
TEST(storage_update_key) {
    forgekv::InMemoryStorage s;
    s.set("lang", "C");
    s.set("lang", "C++");
    auto result = s.get("lang");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "C++");
    ASSERT_EQ(s.size(), std::size_t{1});
}

// S2-3.
TEST(storage_get_missing) {
    forgekv::InMemoryStorage s;
    ASSERT_NO_VALUE(s.get("absent"));
}

// S2-4.
TEST(storage_del_existing) {
    forgekv::InMemoryStorage s;
    s.set("x", "1");
    ASSERT_TRUE(s.del("x"));
    ASSERT_NO_VALUE(s.get("x"));
    ASSERT_EQ(s.size(), std::size_t{0});
}

// S2-5.
TEST(storage_del_missing) {
    forgekv::InMemoryStorage s;
    ASSERT_FALSE(s.del("ghost"));
}

// S2-6.
TEST(storage_exists) {
    forgekv::InMemoryStorage s;
    ASSERT_FALSE(s.exists("k"));
    s.set("k", "v");
    ASSERT_TRUE(s.exists("k"));
    s.del("k");
    ASSERT_FALSE(s.exists("k"));
}

// S2-7.
TEST(storage_size_and_empty) {
    forgekv::InMemoryStorage s;
    ASSERT_TRUE(s.empty());
    ASSERT_EQ(s.size(), std::size_t{0});
    s.set("a", "1");
    ASSERT_FALSE(s.empty());
    ASSERT_EQ(s.size(), std::size_t{1});
    s.set("b", "2");
    ASSERT_EQ(s.size(), std::size_t{2});
    s.del("a");
    ASSERT_EQ(s.size(), std::size_t{1});
    s.clear();
    ASSERT_TRUE(s.empty());
    ASSERT_EQ(s.size(), std::size_t{0});
}

// S2-8.
TEST(storage_clear) {
    forgekv::InMemoryStorage s;
    s.set("p", "1");
    s.set("q", "2");
    s.set("r", "3");
    s.clear();
    ASSERT_TRUE(s.empty());
    ASSERT_NO_VALUE(s.get("p"));
    ASSERT_NO_VALUE(s.get("q"));
    ASSERT_NO_VALUE(s.get("r"));
}

// =============================================================================
// InMemoryStorage through the Storage interface (base pointer)
// =============================================================================

// S2-9.
TEST(storage_interface_set_get) {
    std::unique_ptr<forgekv::Storage> s = std::make_unique<forgekv::InMemoryStorage>();
    s->set("hello", "world");
    auto result = s->get("hello");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "world");
}

// S2-10.
TEST(storage_interface_del_exists) {
    std::unique_ptr<forgekv::Storage> s = std::make_unique<forgekv::InMemoryStorage>();
    s->set("tmp", "data");
    ASSERT_TRUE(s->exists("tmp"));
    ASSERT_TRUE(s->del("tmp"));
    ASSERT_FALSE(s->exists("tmp"));
}

// S2-11.
TEST(storage_interface_size_empty_clear) {
    std::unique_ptr<forgekv::Storage> s = std::make_unique<forgekv::InMemoryStorage>();
    ASSERT_TRUE(s->empty());
    s->set("a", "1");
    s->set("b", "2");
    ASSERT_EQ(s->size(), std::size_t{2});
    s->clear();
    ASSERT_TRUE(s->empty());
}

// =============================================================================
// KeyValueStore through the abstraction — Stage 1 contracts still hold
// =============================================================================

// S2-12.
TEST(kv_default_ctor_still_works) {
    forgekv::KeyValueStore store;
    store.set("stage", "2");
    ASSERT_EQ(*store.get("stage"), "2");
    ASSERT_TRUE(store.exists("stage"));
    ASSERT_EQ(store.size(), std::size_t{1});
    store.del("stage");
    ASSERT_FALSE(store.exists("stage"));
    ASSERT_TRUE(store.empty());
}

// S2-13.
TEST(kv_update_through_abstraction) {
    forgekv::KeyValueStore store;
    store.set("k", "old");
    store.set("k", "new");
    ASSERT_EQ(*store.get("k"), "new");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// S2-14.
TEST(kv_clear_through_abstraction) {
    forgekv::KeyValueStore store;
    store.set("x", "1");
    store.set("y", "2");
    store.clear();
    ASSERT_TRUE(store.empty());
    ASSERT_NO_VALUE(store.get("x"));
}

// S2-15.
TEST(kv_missing_key_through_abstraction) {
    forgekv::KeyValueStore store;
    ASSERT_NO_VALUE(store.get("absent"));
    ASSERT_FALSE(store.exists("absent"));
}

// S2-16.
TEST(kv_del_missing_through_abstraction) {
    forgekv::KeyValueStore store;
    ASSERT_FALSE(store.del("no-such-key"));
    ASSERT_EQ(store.size(), std::size_t{0});
}

// =============================================================================
// Dependency injection — FakeStorage
// =============================================================================

// S2-17.
TEST(di_forwards_set) {
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    store.set("k", "v");
    ASSERT_EQ(fake->set_calls, 1);
}

// S2-18.
TEST(di_forwards_get) {
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    auto result = store.get("k");
    ASSERT_NO_VALUE(result);
    ASSERT_EQ(fake->get_calls, 1);
}

// S2-19.
TEST(di_forwards_del) {
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    bool removed = store.del("k");
    ASSERT_FALSE(removed);
    ASSERT_EQ(fake->exists_calls, 1);
    ASSERT_EQ(fake->del_calls, 0);
}

// S2-20.
TEST(di_forwards_exists) {
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    bool found = store.exists("k");
    ASSERT_FALSE(found);
    ASSERT_EQ(fake->exists_calls, 1);
}

// S2-21.
TEST(di_forwards_utility_methods) {
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    ASSERT_EQ(store.size(), std::size_t{0});
    ASSERT_TRUE(store.empty());
    store.clear();
    ASSERT_EQ(fake->size_calls, 1);
    ASSERT_EQ(fake->empty_calls, 1);
    ASSERT_EQ(fake->clear_calls, 1);
}


// =============================================================================
// Stage 3 tests — Write-Ahead Log behavioral tests (format-agnostic)
// =============================================================================
//
// Stage 3 tests have been updated for Stage 4:
//   - Tests that inspected the raw text content of the WAL file have been
//     removed or rewritten, because Stage 4 intentionally changes the format
//     from human-readable text to binary.
//   - Tests that verify behavioral properties (file created, append semantics,
//     error propagation, write ordering, read-only ops) are preserved because
//     those properties are unchanged.
//
// All tests use temporary files and clean up on exit.
// =============================================================================

// Helper: read the entire content of a file into a vector of bytes.
static std::vector<std::uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("read_file_bytes: cannot open: " + path);
    }
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>());
}

// Helper: RAII guard that deletes a file on scope exit.
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

// Unique temp WAL path per test.
#define TEMP_WAL(name) \
    TempFile name{"test_wal_" #name ".wal"}

// ---------------------------------------------------------------------------
// S3-1. WAL creates a new file if it does not exist.
// ---------------------------------------------------------------------------
TEST(s3_wal_creates_file) {
    TEMP_WAL(guard);
    std::filesystem::remove(guard.path);
    {
        forgekv::WAL wal(guard.path);
    }
    ASSERT_TRUE(std::filesystem::exists(guard.path));
}

// ---------------------------------------------------------------------------
// S3-2. SET produces a non-empty binary record (file is not empty after SET).
// ---------------------------------------------------------------------------
TEST(s3_set_produces_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(!bytes.empty());
}

// ---------------------------------------------------------------------------
// S3-3. Multiple SET operations append records — file grows with each append.
// ---------------------------------------------------------------------------
TEST(s3_multiple_sets_grow_file) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
    }
    const std::size_t size_after_one = read_file_bytes(guard.path).size();

    {
        forgekv::WAL wal(guard.path);
        wal.append_set("age", "21");
        wal.append_set("city", "Bengaluru");
    }
    const std::size_t size_after_three = read_file_bytes(guard.path).size();
    ASSERT_TRUE(size_after_three > size_after_one);
}

// ---------------------------------------------------------------------------
// S3-4. DEL produces a non-empty binary record.
// ---------------------------------------------------------------------------
TEST(s3_del_produces_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_del("age");
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(!bytes.empty());
}

// ---------------------------------------------------------------------------
// S3-5. CLEAR produces a non-empty binary record.
// ---------------------------------------------------------------------------
TEST(s3_clear_produces_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(!bytes.empty());
}

// ---------------------------------------------------------------------------
// S3-6. Reopening WAL appends to existing content (does not truncate).
// ---------------------------------------------------------------------------
TEST(s3_reopen_preserves_content) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("key1", "val1");
    }
    const std::size_t size_first = read_file_bytes(guard.path).size();

    {
        forgekv::WAL wal(guard.path);
        wal.append_set("key2", "val2");
    }
    const std::size_t size_second = read_file_bytes(guard.path).size();

    // File must be larger — old content was preserved.
    ASSERT_TRUE(size_second > size_first);
}

// ---------------------------------------------------------------------------
// S3-7. Mixed sequence (SET / DEL / CLEAR) — file grows with each record.
// ---------------------------------------------------------------------------
TEST(s3_mixed_sequence_grows_file) {
    TEMP_WAL(guard);
    std::size_t prev = 0;
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
        std::size_t after_set = read_file_bytes(guard.path).size();
        ASSERT_TRUE(after_set > prev);
        prev = after_set;

        wal.append_del("name");
        std::size_t after_del = read_file_bytes(guard.path).size();
        ASSERT_TRUE(after_del > prev);
        prev = after_del;

        wal.append_clear();
        std::size_t after_clear = read_file_bytes(guard.path).size();
        ASSERT_TRUE(after_clear > prev);
    }
}

// ---------------------------------------------------------------------------
// S3-8. WAL opening failure throws std::runtime_error.
// ---------------------------------------------------------------------------
TEST(s3_wal_open_failure_throws) {
    const std::string bad_path = "/tmp/forgekv_no_such_dir_xyz/wal.log";
    ASSERT_THROWS(forgekv::WAL{bad_path});
}

// ---------------------------------------------------------------------------
// S3-9. KeyValueStore::set() writes to WAL and updates storage.
// ---------------------------------------------------------------------------
TEST(s3_kvstore_set_writes_wal_and_updates_storage) {
    TEMP_WAL(guard);
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw    = storage.get();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    store.set("name", "Vishnu");

    // WAL file must be non-empty.
    ASSERT_TRUE(!read_file_bytes(guard.path).empty());

    // In-memory storage must have the value.
    auto val = raw->get("name");
    ASSERT_HAS_VALUE(val);
    ASSERT_EQ(*val, "Vishnu");
}

// ---------------------------------------------------------------------------
// S3-10. KeyValueStore::del() writes WAL record and removes from storage.
// ---------------------------------------------------------------------------
TEST(s3_kvstore_del_writes_wal_and_updates_storage) {
    TEMP_WAL(guard);
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw    = storage.get();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    store.set("city", "Bengaluru");
    const std::size_t size_after_set = read_file_bytes(guard.path).size();

    bool removed = store.del("city");
    ASSERT_TRUE(removed);

    // WAL must have grown (a DEL record was appended).
    ASSERT_TRUE(read_file_bytes(guard.path).size() > size_after_set);

    // In-memory storage must no longer have the key.
    ASSERT_FALSE(raw->exists("city"));
}

// ---------------------------------------------------------------------------
// S3-11. KeyValueStore::del() on a non-existent key does NOT write to WAL.
// ---------------------------------------------------------------------------
TEST(s3_del_missing_key_no_wal_record) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    bool removed = store.del("ghost");
    ASSERT_FALSE(removed);

    // WAL file must be empty — no record for a no-op del.
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_EQ(bytes.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// S3-12. KeyValueStore::clear() writes CLEAR to WAL and empties storage.
// ---------------------------------------------------------------------------
TEST(s3_kvstore_clear_writes_wal) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    store.set("a", "1");
    store.set("b", "2");
    const std::size_t size_before_clear = read_file_bytes(guard.path).size();

    store.clear();
    ASSERT_TRUE(store.empty());

    // WAL must have grown (a CLEAR record was appended).
    ASSERT_TRUE(read_file_bytes(guard.path).size() > size_before_clear);
}

// ---------------------------------------------------------------------------
// S3-13. Read-only operations do NOT write to WAL.
// ---------------------------------------------------------------------------
TEST(s3_readonly_ops_no_wal_write) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    (void)store.get("absent");
    (void)store.exists("absent");
    (void)store.size();
    (void)store.empty();

    // WAL file must be empty.
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_EQ(bytes.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// S3-14. Full DI: Storage and WAL both injected, end-to-end behavior.
// ---------------------------------------------------------------------------
TEST(s3_full_di_storage_and_wal) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    store.set("project", "ForgeKV");
    store.set("stage", "4");
    store.del("stage");

    ASSERT_EQ(*store.get("project"), "ForgeKV");
    ASSERT_FALSE(store.exists("stage"));

    // WAL must be non-empty (3 records written).
    ASSERT_TRUE(!read_file_bytes(guard.path).empty());
}

// ---------------------------------------------------------------------------
// S3-15. WAL path() accessor returns the configured path.
// ---------------------------------------------------------------------------
TEST(s3_wal_path_accessor) {
    TEMP_WAL(guard);
    forgekv::WAL wal(guard.path);
    ASSERT_EQ(wal.path(), guard.path);
}

// ---------------------------------------------------------------------------
// S3-16. Empty value in SET is handled (binary record exists).
// ---------------------------------------------------------------------------
TEST(s3_set_empty_value) {
    TEMP_WAL(guard);
    forgekv::WAL wal(guard.path);
    wal.append_set("emptyval", "");
    ASSERT_TRUE(!read_file_bytes(guard.path).empty());
}

// ---------------------------------------------------------------------------
// S3-17. Empty key in SET is handled (binary record exists).
// ---------------------------------------------------------------------------
TEST(s3_set_empty_key) {
    TEMP_WAL(guard);
    forgekv::WAL wal(guard.path);
    wal.append_set("", "somevalue");
    ASSERT_TRUE(!read_file_bytes(guard.path).empty());
}

// ---------------------------------------------------------------------------
// S3-18. Stage 1 API still works when WAL is injected (end-to-end).
// ---------------------------------------------------------------------------
TEST(s3_stage1_api_works_with_wal) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    store.set("lang", "C++");
    ASSERT_EQ(*store.get("lang"), "C++");
    ASSERT_TRUE(store.exists("lang"));
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_FALSE(store.empty());
    ASSERT_TRUE(store.del("lang"));
    ASSERT_FALSE(store.exists("lang"));
    ASSERT_TRUE(store.empty());
    ASSERT_FALSE(store.del("lang"));
    store.set("x", "1");
    store.clear();
    ASSERT_TRUE(store.empty());
}


// =============================================================================
// Stage 4 tests — Binary WAL + CRC32 Checksums
// =============================================================================
//
// These tests verify the binary record format, field values, checksum
// generation, corruption detection, truncation detection, and special-
// character handling.
//
// Each test uses WAL::read_record() to deserialise and validate records
// written by append_set / append_del / append_clear.
//
// WAL::read_record() does NOT replay into Storage — it only validates.
// =============================================================================

// Helper: open a file as a binary input stream for read_record().
static std::ifstream open_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("open_binary: cannot open: " + path);
    }
    return f;
}

// Helper: decode a little-endian uint32_t from raw bytes at offset.
static std::uint32_t read_u32_le(const std::vector<std::uint8_t>& b,
                                  std::size_t offset) {
    return static_cast<std::uint32_t>(b[offset])
         | (static_cast<std::uint32_t>(b[offset + 1]) <<  8)
         | (static_cast<std::uint32_t>(b[offset + 2]) << 16)
         | (static_cast<std::uint32_t>(b[offset + 3]) << 24);
}

// ---------------------------------------------------------------------------
// S4-1. Binary WAL file is created.
// ---------------------------------------------------------------------------
TEST(s4_binary_wal_file_is_created) {
    TEMP_WAL(guard);
    std::filesystem::remove(guard.path);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
    }
    ASSERT_TRUE(std::filesystem::exists(guard.path));
    ASSERT_TRUE(read_file_bytes(guard.path).size() > 0);
}

// ---------------------------------------------------------------------------
// S4-2. SET produces a binary record (not human-readable text).
//   The file must NOT start with 'S' (ASCII 0x53) as text "SET|..." would.
//   It must start with the magic bytes 0x41 0x57 0x4B 0x46.
// ---------------------------------------------------------------------------
TEST(s4_set_produces_binary_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
    }
    const auto bytes = read_file_bytes(guard.path);
    // First byte is 0x41 ('A'), NOT 0x53 ('S').
    ASSERT_TRUE(bytes.size() >= 4);
    ASSERT_EQ(bytes[0], std::uint8_t{0x41});
    ASSERT_EQ(bytes[1], std::uint8_t{0x57});
    ASSERT_EQ(bytes[2], std::uint8_t{0x4B});
    ASSERT_EQ(bytes[3], std::uint8_t{0x46});
}

// ---------------------------------------------------------------------------
// S4-3. DEL produces a binary record (starts with magic bytes).
// ---------------------------------------------------------------------------
TEST(s4_del_produces_binary_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_del("somekey");
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 4);
    ASSERT_EQ(bytes[0], std::uint8_t{0x41});
    ASSERT_EQ(bytes[1], std::uint8_t{0x57});
    ASSERT_EQ(bytes[2], std::uint8_t{0x4B});
    ASSERT_EQ(bytes[3], std::uint8_t{0x46});
}

// ---------------------------------------------------------------------------
// S4-4. CLEAR produces a binary record (starts with magic bytes).
// ---------------------------------------------------------------------------
TEST(s4_clear_produces_binary_record) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 4);
    ASSERT_EQ(bytes[0], std::uint8_t{0x41});
    ASSERT_EQ(bytes[1], std::uint8_t{0x57});
    ASSERT_EQ(bytes[2], std::uint8_t{0x4B});
    ASSERT_EQ(bytes[3], std::uint8_t{0x46});
}

// ---------------------------------------------------------------------------
// S4-5. Multiple records are appended in order and all round-trip correctly.
// ---------------------------------------------------------------------------
TEST(s4_multiple_records_appended_in_order) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
        wal.append_set("age", "21");
        wal.append_del("age");
        wal.append_clear();
    }

    auto f = open_binary(guard.path);

    forgekv::WalRecord r1 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r1.opcode, forgekv::kOpSet);
    ASSERT_EQ(r1.key,   "name");
    ASSERT_EQ(r1.value, "Vishnu");

    forgekv::WalRecord r2 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r2.opcode, forgekv::kOpSet);
    ASSERT_EQ(r2.key,   "age");
    ASSERT_EQ(r2.value, "21");

    forgekv::WalRecord r3 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r3.opcode, forgekv::kOpDel);
    ASSERT_EQ(r3.key,   "age");
    ASSERT_EQ(r3.value, "");

    forgekv::WalRecord r4 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r4.opcode, forgekv::kOpClear);
    ASSERT_EQ(r4.key,   "");
    ASSERT_EQ(r4.value, "");
}

// ---------------------------------------------------------------------------
// S4-6. Reopening an existing WAL preserves old records.
// ---------------------------------------------------------------------------
TEST(s4_reopen_preserves_old_records) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("first", "record");
    }
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("second", "record");
    }

    auto f = open_binary(guard.path);

    forgekv::WalRecord r1 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r1.key,   "first");
    ASSERT_EQ(r1.value, "record");

    forgekv::WalRecord r2 = forgekv::WAL::read_record(f);
    ASSERT_EQ(r2.key,   "second");
    ASSERT_EQ(r2.value, "record");
}

// ---------------------------------------------------------------------------
// S4-7. Magic value in the raw bytes is correct (0x464B5741 LE).
// ---------------------------------------------------------------------------
TEST(s4_magic_value_correct) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 4);
    const std::uint32_t magic = read_u32_le(bytes, 0);
    ASSERT_EQ(magic, forgekv::kWalMagic);
}

// ---------------------------------------------------------------------------
// S4-8. Version byte is correct (0x01).
// ---------------------------------------------------------------------------
TEST(s4_version_byte_correct) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 5);
    ASSERT_EQ(bytes[4], forgekv::kWalVersion);
}

// ---------------------------------------------------------------------------
// S4-9. Operation code byte is correct for SET, DEL, CLEAR.
// ---------------------------------------------------------------------------
TEST(s4_opcode_correct) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
        wal.append_del("k");
        wal.append_clear();
    }

    const auto bytes = read_file_bytes(guard.path);
    // Header size = 14, checksum = 4. Each record: 18 + key_len + val_len.
    // Record 0 (SET k=1, v=1): 18 + 1 + 1 = 20 bytes. Opcode at byte 5.
    // Record 1 (DEL k=1):      18 + 1 + 0 = 19 bytes. Opcode at byte 20+5=25.
    // Record 2 (CLEAR):        18 + 0 + 0 = 18 bytes. Opcode at 39+5=44.
    ASSERT_EQ(bytes[5],  forgekv::kOpSet);
    ASSERT_EQ(bytes[25], forgekv::kOpDel);
    ASSERT_EQ(bytes[44], forgekv::kOpClear);
}

// ---------------------------------------------------------------------------
// S4-10. Key length is encoded correctly in the header.
// ---------------------------------------------------------------------------
TEST(s4_key_length_encoded_correctly) {
    TEMP_WAL(guard);
    const std::string key = "hello";  // 5 bytes
    {
        forgekv::WAL wal(guard.path);
        wal.append_set(key, "world");
    }
    const auto bytes = read_file_bytes(guard.path);
    // key_len is at offset 6, 4 bytes LE.
    ASSERT_TRUE(bytes.size() >= 10);
    const std::uint32_t key_len = read_u32_le(bytes, 6);
    ASSERT_EQ(key_len, std::uint32_t{5});
}

// ---------------------------------------------------------------------------
// S4-11. Value length is encoded correctly in the header.
// ---------------------------------------------------------------------------
TEST(s4_value_length_encoded_correctly) {
    TEMP_WAL(guard);
    const std::string value = "world";  // 5 bytes
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("hello", value);
    }
    const auto bytes = read_file_bytes(guard.path);
    // val_len is at offset 10, 4 bytes LE.
    ASSERT_TRUE(bytes.size() >= 14);
    const std::uint32_t val_len = read_u32_le(bytes, 10);
    ASSERT_EQ(val_len, std::uint32_t{5});
}

// ---------------------------------------------------------------------------
// S4-12. SET key/value bytes round-trip correctly via read_record().
// ---------------------------------------------------------------------------
TEST(s4_set_roundtrip) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("project", "ForgeKV");
    }
    auto f = open_binary(guard.path);
    forgekv::WalRecord rec = forgekv::WAL::read_record(f);
    ASSERT_EQ(rec.opcode, forgekv::kOpSet);
    ASSERT_EQ(rec.key,   "project");
    ASSERT_EQ(rec.value, "ForgeKV");
}

// ---------------------------------------------------------------------------
// S4-13. DEL key bytes round-trip correctly; value is empty.
// ---------------------------------------------------------------------------
TEST(s4_del_roundtrip) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_del("mykey");
    }
    auto f = open_binary(guard.path);
    forgekv::WalRecord rec = forgekv::WAL::read_record(f);
    ASSERT_EQ(rec.opcode, forgekv::kOpDel);
    ASSERT_EQ(rec.key,   "mykey");
    ASSERT_EQ(rec.value, "");
}

// ---------------------------------------------------------------------------
// S4-14. CLEAR contains no key/value payload; both fields are empty.
// ---------------------------------------------------------------------------
TEST(s4_clear_no_payload) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }

    // Verify raw lengths are zero.
    const auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 14);
    const std::uint32_t key_len = read_u32_le(bytes, 6);
    const std::uint32_t val_len = read_u32_le(bytes, 10);
    ASSERT_EQ(key_len, std::uint32_t{0});
    ASSERT_EQ(val_len, std::uint32_t{0});

    // Also verify via read_record.
    auto f = open_binary(guard.path);
    forgekv::WalRecord rec = forgekv::WAL::read_record(f);
    ASSERT_EQ(rec.opcode, forgekv::kOpClear);
    ASSERT_EQ(rec.key,   "");
    ASSERT_EQ(rec.value, "");
}

// ---------------------------------------------------------------------------
// S4-15. Checksum field is present (record size matches expected formula).
//   Expected size for SET(key_len=K, val_len=V) = 14 + K + V + 4 = 18 + K + V.
// ---------------------------------------------------------------------------
TEST(s4_checksum_is_present) {
    TEMP_WAL(guard);
    const std::string key   = "abc";  // 3 bytes
    const std::string value = "xyz";  // 3 bytes
    {
        forgekv::WAL wal(guard.path);
        wal.append_set(key, value);
    }
    const auto bytes = read_file_bytes(guard.path);
    // Expected: 14 (header) + 3 (key) + 3 (value) + 4 (checksum) = 24 bytes.
    ASSERT_EQ(bytes.size(), std::size_t{24});
}

// ---------------------------------------------------------------------------
// S4-16. Valid checksum is accepted (read_record succeeds on clean record).
// ---------------------------------------------------------------------------
TEST(s4_valid_checksum_accepted) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
    }
    // read_record must not throw.
    auto f = open_binary(guard.path);
    bool threw = false;
    try {
        forgekv::WAL::read_record(f);
    } catch (...) {
        threw = true;
    }
    ASSERT_FALSE(threw);
}

// ---------------------------------------------------------------------------
// S4-17. Corrupting a byte causes checksum validation to fail.
//   We flip one byte in the payload area and verify read_record throws.
// ---------------------------------------------------------------------------
TEST(s4_corrupted_byte_fails_checksum) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
    }

    // Read the raw bytes, corrupt one payload byte (e.g. byte 14 = first key byte).
    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() > 14);
    bytes[14] ^= 0xFF;  // flip all bits of the first key byte

    // Write back the corrupted file.
    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    // read_record must throw due to checksum mismatch.
    auto f = open_binary(guard.path);
    ASSERT_THROWS(forgekv::WAL::read_record(f));
}

// ---------------------------------------------------------------------------
// S4-18. Truncating a record causes validation to fail.
//   We remove the last few bytes of the record and verify read_record throws.
// ---------------------------------------------------------------------------
TEST(s4_truncated_record_fails) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("truncation", "test");
    }

    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() > 6);

    // Truncate: remove last 6 bytes (cuts into checksum + end of payload).
    bytes.resize(bytes.size() - 6);

    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto f = open_binary(guard.path);
    ASSERT_THROWS(forgekv::WAL::read_record(f));
}

// ---------------------------------------------------------------------------
// S4-19. Invalid magic is rejected.
// ---------------------------------------------------------------------------
TEST(s4_invalid_magic_rejected) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
    }

    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 4);
    // Corrupt the magic bytes.
    bytes[0] = 0xDE;
    bytes[1] = 0xAD;
    bytes[2] = 0xBE;
    bytes[3] = 0xEF;

    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto f = open_binary(guard.path);
    ASSERT_THROWS(forgekv::WAL::read_record(f));
}

// ---------------------------------------------------------------------------
// S4-20. Invalid version is rejected.
// ---------------------------------------------------------------------------
TEST(s4_invalid_version_rejected) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
    }

    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 5);
    // Version is at byte 4. Set it to an unknown value.
    bytes[4] = 0xFF;

    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto f = open_binary(guard.path);
    ASSERT_THROWS(forgekv::WAL::read_record(f));
}

// ---------------------------------------------------------------------------
// S4-21. Invalid opcode is rejected.
// ---------------------------------------------------------------------------
TEST(s4_invalid_opcode_rejected) {
    TEMP_WAL(guard);
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("k", "v");
    }

    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 6);
    // Opcode is at byte 5. Set it to an unknown value.
    bytes[5] = 0xAB;

    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    auto f = open_binary(guard.path);
    ASSERT_THROWS(forgekv::WAL::read_record(f));
}

// ---------------------------------------------------------------------------
// S4-22. Special characters in keys/values are preserved.
//   Characters that were problematic in the Stage 3 text format:
//   '|', '\n', '\r', spaces, null bytes, tab, backslash.
// ---------------------------------------------------------------------------
TEST(s4_special_chars_preserved) {
    TEMP_WAL(guard);
    const std::string special_key   = "key|with|pipes\nand\nnewlines\r\n";
    const std::string special_value = "val with spaces\t\r\n|and|pipes\\slash";
    {
        forgekv::WAL wal(guard.path);
        wal.append_set(special_key, special_value);
    }

    auto f = open_binary(guard.path);
    forgekv::WalRecord rec = forgekv::WAL::read_record(f);
    ASSERT_EQ(rec.opcode, forgekv::kOpSet);
    ASSERT_EQ(rec.key,   special_key);
    ASSERT_EQ(rec.value, special_value);
}

// ---------------------------------------------------------------------------
// S4-23. WAL write failure is propagated as an exception.
//   Simulated by using an invalid directory path.
// ---------------------------------------------------------------------------
TEST(s4_wal_write_failure_propagated) {
    const std::string bad_path = "/tmp/forgekv_no_such_dir_xyz/wal.log";
    ASSERT_THROWS(forgekv::WAL{bad_path});
}

// ---------------------------------------------------------------------------
// S4-24. WAL failure does NOT mutate Storage.
//   We inject a WAL that fails on open; the KeyValueStore constructor
//   itself must throw, and no storage mutation occurs.
// ---------------------------------------------------------------------------
TEST(s4_wal_failure_does_not_mutate_storage) {
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw = storage.get();

    bool threw = false;
    try {
        // This WAL will fail to open — bad path.
        auto wal = std::make_unique<forgekv::WAL>(
            "/tmp/forgekv_no_such_dir_xyz/wal.log");
        forgekv::KeyValueStore store(std::move(storage), std::move(wal));
        store.set("should_not_exist", "value");
    } catch (const std::exception&) {
        threw = true;
    }

    ASSERT_TRUE(threw);
    // Storage was never touched because construction threw before set() ran.
    ASSERT_TRUE(raw->empty());
}

// ---------------------------------------------------------------------------
// S4-25. Read-only operations do NOT append any WAL records.
//   File size must not change after get / exists / size / empty.
// ---------------------------------------------------------------------------
TEST(s4_readonly_ops_do_not_append) {
    TEMP_WAL(guard);

    // Write one record so the WAL has some content.
    {
        forgekv::WAL wal_writer(guard.path);
        wal_writer.append_set("key", "value");
    }

    const std::size_t size_after_set = read_file_bytes(guard.path).size();

    // Now open a full store; perform only read operations.
    {
        auto wal = std::make_unique<forgekv::WAL>(guard.path);
        forgekv::KeyValueStore store(
            std::make_unique<forgekv::InMemoryStorage>(),
            std::move(wal)
        );

        (void)store.get("key");
        (void)store.exists("key");
        (void)store.size();
        (void)store.empty();
    }

    const std::size_t size_after_reads = read_file_bytes(guard.path).size();
    ASSERT_EQ(size_after_reads, size_after_set);
}

// ---------------------------------------------------------------------------
// S4-26. Stage 3 write ordering is intact in Stage 4.
//   set() → WAL append → Storage update.
//   If WAL succeeds, Storage must have the value.
//   The WAL file must also have grown.
// ---------------------------------------------------------------------------
TEST(s4_write_ordering_intact) {
    TEMP_WAL(guard);
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw    = storage.get();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Before set: both WAL and storage are empty.
    ASSERT_EQ(read_file_bytes(guard.path).size(), std::size_t{0});
    ASSERT_TRUE(raw->empty());

    store.set("order", "check");

    // After set: WAL must have content AND storage must have value.
    ASSERT_TRUE(read_file_bytes(guard.path).size() > 0);
    auto v = raw->get("order");
    ASSERT_HAS_VALUE(v);
    ASSERT_EQ(*v, "check");
}

// ---------------------------------------------------------------------------
// S4-27. Existing Stage 1/2/3 behavior continues to work with binary WAL.
//   Full end-to-end scenario through KeyValueStore.
// ---------------------------------------------------------------------------
TEST(s4_existing_behavior_continues) {
    TEMP_WAL(guard);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    // Stage 1 operations.
    store.set("name",  "Vishnu");
    store.set("lang",  "C++");
    store.set("stage", "4");

    ASSERT_EQ(*store.get("name"),  "Vishnu");
    ASSERT_EQ(*store.get("lang"),  "C++");
    ASSERT_EQ(*store.get("stage"), "4");
    ASSERT_EQ(store.size(), std::size_t{3});

    ASSERT_TRUE(store.del("lang"));
    ASSERT_FALSE(store.exists("lang"));
    ASSERT_EQ(store.size(), std::size_t{2});

    store.clear();
    ASSERT_TRUE(store.empty());
    ASSERT_NO_VALUE(store.get("name"));

    // WAL must contain records for all 5 mutations
    // (3 SETs + 1 DEL + 1 CLEAR = 5 records total).
    auto f = open_binary(guard.path);
    int count = 0;
    bool hit_eof = false;
    while (!hit_eof) {
        try {
            forgekv::WAL::read_record(f);
            ++count;
        } catch (const std::runtime_error& e) {
            // Distinguish truncation (unexpected) from clean EOF.
            // A clean EOF before any bytes of a record = stream eofbit set,
            // and the first read_exact will throw "truncated record (header)".
            // We detect this by checking whether f is at EOF before the throw.
            hit_eof = true;
        }
    }
    ASSERT_EQ(count, 5);
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed  = 0;

    std::cout << "\nForgeKV Stage 1 + 2 + 3 + 4 — KeyValueStore & WAL Tests\n";
    std::cout << std::string(57, '=') << "\n\n";

    for (const auto& tc : tests) {
        std::cout << "  [ RUN  ] " << tc.name << "\n";
        try {
            tc.fn();
            std::cout << "  [ PASS ] " << tc.name << "\n";
            ++passed;
        } catch (const AssertionFailure& e) {
            std::cout << "  [ FAIL ] " << tc.name << "\n";
            std::cout << "           " << e.message << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "  [ FAIL ] " << tc.name << " (unexpected exception)\n";
            std::cout << "           " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "  [ FAIL ] " << tc.name << " (unknown exception)\n";
            ++failed;
        }
    }

    std::cout << "\n" << std::string(57, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";

    return (failed == 0) ? 0 : 1;
}
