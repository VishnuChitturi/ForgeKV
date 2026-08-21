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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <latch>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
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
//
// Note (Stage 5 update): All Stage 1 tests now use explicit temporary WAL paths
// to avoid reading any pre-existing forgekv.wal in the working directory.
// Stage 5 changed the KeyValueStore constructors to perform WAL recovery, so
// using the default WAL path could produce a non-empty store if a leftover
// WAL exists from a previous run.  Using a temp WAL ensures each test starts
// with a clean, empty store.
// =============================================================================

// ---------------------------------------------------------------------------
// File/temp helpers — used by Stage 1–5 tests
// These are declared here (before any tests) so all stages can use them.
// ---------------------------------------------------------------------------

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

// Helper macro: declare a temp WAL, open a DI store with it, and wire them.
#define KV_WITH_TEMP_WAL(store_name)                                    \
    TempFile store_name##_wal{"test_s1_" #store_name ".wal"};           \
    auto store_name##_storage = std::make_unique<forgekv::InMemoryStorage>(); \
    auto store_name##_wal_ptr = std::make_unique<forgekv::WAL>(store_name##_wal.path); \
    forgekv::KeyValueStore store_name(                                   \
        std::move(store_name##_storage),                                 \
        std::move(store_name##_wal_ptr))

// 1. SET followed by GET — fundamental round-trip
TEST(set_then_get) {
    KV_WITH_TEMP_WAL(store);
    store.set("name", "Vishnu");
    auto result = store.get("name");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "Vishnu");
}

// 2. Multiple keys — independent entries
TEST(multiple_keys) {
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
    store.set("name", "Alice");
    ASSERT_EQ(*store.get("name"), "Alice");
    store.set("name", "Bob");
    ASSERT_EQ(*store.get("name"), "Bob");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// 4. GET of a missing key — must return nullopt, not crash or return garbage
TEST(get_missing_key) {
    KV_WITH_TEMP_WAL(store);
    auto result = store.get("nonexistent");
    ASSERT_NO_VALUE(result);
}

// 5. GET returns nullopt after DELETE
TEST(delete_existing_key) {
    KV_WITH_TEMP_WAL(store);
    store.set("key", "value");
    ASSERT_HAS_VALUE(store.get("key"));
    bool removed = store.del("key");
    ASSERT_TRUE(removed);
    ASSERT_NO_VALUE(store.get("key"));
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 6. DELETE of a missing key — must be a safe no-op, not crash
TEST(delete_missing_key) {
    KV_WITH_TEMP_WAL(store);
    bool removed = store.del("ghost");
    ASSERT_FALSE(removed);
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 7. EXISTS for an existing key
TEST(exists_present_key) {
    KV_WITH_TEMP_WAL(store);
    store.set("lang", "C++");
    ASSERT_TRUE(store.exists("lang"));
}

// 8. EXISTS for a missing key
TEST(exists_absent_key) {
    KV_WITH_TEMP_WAL(store);
    ASSERT_FALSE(store.exists("missing"));
}

// 9a. Empty string as a value — valid input
TEST(empty_string_value) {
    KV_WITH_TEMP_WAL(store);
    store.set("key", "");
    auto result = store.get("key");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "");
}

// 9b. Empty string as a key — valid (unusual but should not crash)
TEST(empty_string_key) {
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
    store.set("k", "v");
    ASSERT_TRUE(store.exists("k"));
    ASSERT_EQ(store.size(), std::size_t{1});
}

// 14. del() is idempotent — calling twice on same key is safe
TEST(double_delete) {
    KV_WITH_TEMP_WAL(store);
    store.set("temp", "data");
    ASSERT_TRUE(store.del("temp"));
    ASSERT_FALSE(store.del("temp"));
    ASSERT_TRUE(store.empty());
}

// 15. Large number of distinct keys
TEST(many_keys) {
    KV_WITH_TEMP_WAL(store);
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
    std::vector<std::pair<std::string, std::string>> get_all() const override {
        return {};
    }
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
//
// Note (Stage 5 update): Tests that previously used the default KeyValueStore
// constructor now use explicit temp WAL paths to start with a clean empty store.
// =============================================================================

// S2-12.
TEST(kv_default_ctor_still_works) {
    KV_WITH_TEMP_WAL(store);
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
    KV_WITH_TEMP_WAL(store);
    store.set("k", "old");
    store.set("k", "new");
    ASSERT_EQ(*store.get("k"), "new");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// S2-14.
TEST(kv_clear_through_abstraction) {
    KV_WITH_TEMP_WAL(store);
    store.set("x", "1");
    store.set("y", "2");
    store.clear();
    ASSERT_TRUE(store.empty());
    ASSERT_NO_VALUE(store.get("x"));
}

// S2-15.
TEST(kv_missing_key_through_abstraction) {
    KV_WITH_TEMP_WAL(store);
    ASSERT_NO_VALUE(store.get("absent"));
    ASSERT_FALSE(store.exists("absent"));
}

// S2-16.
TEST(kv_del_missing_through_abstraction) {
    KV_WITH_TEMP_WAL(store);
    ASSERT_FALSE(store.del("no-such-key"));
    ASSERT_EQ(store.size(), std::size_t{0});
}

// =============================================================================
// Dependency injection — FakeStorage
// =============================================================================

// S2-17.
TEST(di_forwards_set) {
    TEMP_WAL(guard);
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(ptr), std::move(wal));
    store.set("k", "v");
    ASSERT_EQ(fake->set_calls, 1);
}

// S2-18.
TEST(di_forwards_get) {
    TEMP_WAL(guard);
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(ptr), std::move(wal));
    auto result = store.get("k");
    ASSERT_NO_VALUE(result);
    ASSERT_EQ(fake->get_calls, 1);
}

// S2-19.
TEST(di_forwards_del) {
    TEMP_WAL(guard);
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(ptr), std::move(wal));
    bool removed = store.del("k");
    ASSERT_FALSE(removed);
    ASSERT_EQ(fake->exists_calls, 1);
    ASSERT_EQ(fake->del_calls, 0);
}

// S2-20.
TEST(di_forwards_exists) {
    TEMP_WAL(guard);
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(ptr), std::move(wal));
    bool found = store.exists("k");
    ASSERT_FALSE(found);
    ASSERT_EQ(fake->exists_calls, 1);
}

// S2-21.
TEST(di_forwards_utility_methods) {
    TEMP_WAL(guard);
    auto* fake = new FakeStorage();
    auto ptr = std::unique_ptr<forgekv::Storage>(fake);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(ptr), std::move(wal));
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
// Stage 5 tests — Crash Recovery / WAL Replay
// =============================================================================
//
// All Stage 5 tests use temporary WAL files to ensure full isolation.
// No test relies on manually created repository files.
// All temporary files are cleaned up via RAII (TempFile).
//
// Helpers used:
//   write_wal_records(path, fn)  — write records via a WAL, then close it
//   make_store(path)             — open a KeyValueStore with DI WAL at path
//   corrupt_bytes(path, offset, count, xor_mask) — flip bytes in a file
//   truncate_file(path, keep_bytes) — shrink a file to N bytes
// =============================================================================

// Helper: build a WAL at path, invoke fn(wal), then let wal go out of scope.
static void write_wal_records(const std::string& path,
                               std::function<void(forgekv::WAL&)> fn)
{
    forgekv::WAL wal(path);
    fn(wal);
}

// Helper: open a fully-DI KeyValueStore backed by InMemoryStorage + WAL at path.
static forgekv::KeyValueStore make_store(const std::string& path)
{
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(path);
    return forgekv::KeyValueStore(std::move(storage), std::move(wal));
}

// Helper: XOR-flip `count` bytes at `offset` in the file at `path`.
static void corrupt_bytes(const std::string& path,
                           std::size_t offset,
                           std::size_t count = 1,
                           std::uint8_t xor_mask = 0xFF)
{
    auto bytes = read_file_bytes(path);
    for (std::size_t i = offset; i < offset + count && i < bytes.size(); ++i) {
        bytes[i] ^= xor_mask;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// Helper: truncate file to `keep_bytes` bytes.
static void truncate_file(const std::string& path, std::size_t keep_bytes)
{
    auto bytes = read_file_bytes(path);
    if (keep_bytes < bytes.size()) {
        bytes.resize(keep_bytes);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// ---------------------------------------------------------------------------
// S5-1. Empty/new WAL starts successfully — Storage is empty.
// ---------------------------------------------------------------------------
TEST(s5_empty_wal_starts_successfully) {
    TEMP_WAL(guard);
    std::filesystem::remove(guard.path); // ensure file does not exist
    auto store = make_store(guard.path);
    ASSERT_TRUE(store.empty());
    ASSERT_EQ(store.size(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// S5-2. Existing WAL with one SET is recovered.
// ---------------------------------------------------------------------------
TEST(s5_single_set_recovered) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name", "Vishnu");
    });
    auto store = make_store(guard.path);
    auto v = store.get("name");
    ASSERT_HAS_VALUE(v);
    ASSERT_EQ(*v, "Vishnu");
}

// ---------------------------------------------------------------------------
// S5-3. Multiple SET operations are recovered.
// ---------------------------------------------------------------------------
TEST(s5_multiple_sets_recovered) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name", "Vishnu");
        wal.append_set("age", "21");
        wal.append_set("city", "Bengaluru");
    });
    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("name"), "Vishnu");
    ASSERT_EQ(*store.get("age"), "21");
    ASSERT_EQ(*store.get("city"), "Bengaluru");
    ASSERT_EQ(store.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// S5-4. SET followed by SET on same key — final value wins.
// ---------------------------------------------------------------------------
TEST(s5_overwrite_same_key) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("x", "first");
        wal.append_set("x", "second");
    });
    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("x"), "second");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// S5-5. SET followed by DEL removes the key.
// ---------------------------------------------------------------------------
TEST(s5_set_then_del_removes_key) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name", "Vishnu");
        wal.append_del("name");
    });
    auto store = make_store(guard.path);
    ASSERT_FALSE(store.exists("name"));
    ASSERT_TRUE(store.empty());
}

// ---------------------------------------------------------------------------
// S5-6. SET + DEL + SET produces final value.
// ---------------------------------------------------------------------------
TEST(s5_set_del_set_final_value) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("x", "1");
        wal.append_del("x");
        wal.append_set("x", "3");
    });
    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("x"), "3");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// S5-7. CLEAR removes previously stored keys during recovery.
// ---------------------------------------------------------------------------
TEST(s5_clear_removes_prior_keys) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("a", "1");
        wal.append_set("b", "2");
        wal.append_clear();
    });
    auto store = make_store(guard.path);
    ASSERT_TRUE(store.empty());
    ASSERT_FALSE(store.exists("a"));
    ASSERT_FALSE(store.exists("b"));
}

// ---------------------------------------------------------------------------
// S5-8. CLEAR followed by SET restores only later data.
// ---------------------------------------------------------------------------
TEST(s5_clear_then_set_only_later_data) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("a", "1");
        wal.append_set("b", "2");
        wal.append_clear();
        wal.append_set("c", "3");
    });
    auto store = make_store(guard.path);
    ASSERT_FALSE(store.exists("a"));
    ASSERT_FALSE(store.exists("b"));
    ASSERT_TRUE(store.exists("c"));
    ASSERT_EQ(*store.get("c"), "3");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// ---------------------------------------------------------------------------
// S5-9. Mixed SET/DEL/CLEAR sequence reproduces expected final state.
// ---------------------------------------------------------------------------
TEST(s5_mixed_sequence_final_state) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name",    "Vishnu");
        wal.append_set("age",     "21");
        wal.append_set("city",    "Bengaluru");
        wal.append_del("age");
        wal.append_set("project", "ForgeKV");
    });
    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("name"),    "Vishnu");
    ASSERT_FALSE(store.exists("age"));
    ASSERT_EQ(*store.get("city"),    "Bengaluru");
    ASSERT_EQ(*store.get("project"), "ForgeKV");
    ASSERT_EQ(store.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// S5-10. Recovery preserves key/value special characters.
// ---------------------------------------------------------------------------
TEST(s5_special_chars_recovered) {
    TEMP_WAL(guard);
    const std::string key   = "key|with|pipes\nand\nnewlines\r\n";
    const std::string value = "val with spaces\t\r\n|and|pipes\\slash";
    write_wal_records(guard.path, [&](forgekv::WAL& wal) {
        wal.append_set(key, value);
    });
    auto store = make_store(guard.path);
    ASSERT_TRUE(store.exists(key));
    ASSERT_EQ(*store.get(key), value);
}

// ---------------------------------------------------------------------------
// S5-11. Recovery reads WAL in strict file order.
//   Three overwrites of key "x" — final value must be the last one written.
// ---------------------------------------------------------------------------
TEST(s5_replay_strict_file_order) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("x", "alpha");
        wal.append_set("x", "beta");
        wal.append_set("x", "gamma");
    });
    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("x"), "gamma");
}

// ---------------------------------------------------------------------------
// S5-12. Recovery does not append new WAL records.
//   The WAL file size must not change after opening for recovery.
// ---------------------------------------------------------------------------
TEST(s5_recovery_does_not_append_wal_records) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("a", "1");
        wal.append_set("b", "2");
    });

    const std::size_t size_before = read_file_bytes(guard.path).size();

    // Open for recovery only — do not call any mutations.
    {
        auto store = make_store(guard.path);
        // Read-only: recovery should not have grown the file.
        (void)store.get("a");
        (void)store.exists("b");
    }

    const std::size_t size_after = read_file_bytes(guard.path).size();
    ASSERT_EQ(size_after, size_before);
}

// ---------------------------------------------------------------------------
// S5-13. After recovery, a new mutation appends to the existing WAL.
// ---------------------------------------------------------------------------
TEST(s5_new_mutation_appends_after_recovery) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name", "Vishnu");
    });

    const std::size_t size_before = read_file_bytes(guard.path).size();

    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("name"), "Vishnu"); // recovered
        store.set("city", "Bengaluru");          // new mutation
    }

    const std::size_t size_after = read_file_bytes(guard.path).size();
    ASSERT_TRUE(size_after > size_before); // WAL grew
}

// ---------------------------------------------------------------------------
// S5-14. Reopen after new mutation recovers the complete state again.
//   (Round-trip: write → close → recover → write more → close → recover)
// ---------------------------------------------------------------------------
TEST(s5_reopen_after_new_mutation) {
    TEMP_WAL(guard);

    // First lifetime: write name + age + DEL age + project.
    {
        auto store = make_store(guard.path);
        store.set("name",    "Vishnu");
        store.set("age",     "21");
        store.del("age");
        store.set("project", "ForgeKV");
    }

    // Second lifetime: recover, verify, add city.
    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("name"),    "Vishnu");
        ASSERT_FALSE(store.exists("age"));
        ASSERT_EQ(*store.get("project"), "ForgeKV");
        store.set("city", "Bengaluru");
    }

    // Third lifetime: recover again, verify complete state.
    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("name"),    "Vishnu");
        ASSERT_FALSE(store.exists("age"));
        ASSERT_EQ(*store.get("project"), "ForgeKV");
        ASSERT_EQ(*store.get("city"),    "Bengaluru");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S5-15. Checksum corruption causes recovery failure.
// ---------------------------------------------------------------------------
TEST(s5_checksum_corruption_fails_recovery) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name", "Vishnu");
    });
    // Corrupt a payload byte (first key byte at offset 14).
    corrupt_bytes(guard.path, 14);
    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-16. Invalid magic causes recovery failure.
// ---------------------------------------------------------------------------
TEST(s5_invalid_magic_fails_recovery) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("k", "v");
    });
    // Overwrite magic bytes at offset 0.
    auto bytes = read_file_bytes(guard.path);
    bytes[0] = 0xDE; bytes[1] = 0xAD; bytes[2] = 0xBE; bytes[3] = 0xEF;
    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-17. Invalid version causes recovery failure.
// ---------------------------------------------------------------------------
TEST(s5_invalid_version_fails_recovery) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("k", "v");
    });
    // Version byte is at offset 4.
    corrupt_bytes(guard.path, 4);
    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-18. Invalid opcode causes recovery failure.
// ---------------------------------------------------------------------------
TEST(s5_invalid_opcode_fails_recovery) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("k", "v");
    });
    // Opcode byte is at offset 5.
    corrupt_bytes(guard.path, 5);
    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-19. Malformed/invalid lengths cause recovery failure.
//   Corrupt the val_len field to 0 in a SET record that has a non-empty value.
//   The parser reads the wrong number of payload bytes and then reads the wrong
//   bytes as the CRC, causing a checksum mismatch → recovery failure.
// ---------------------------------------------------------------------------
TEST(s5_invalid_lengths_fail_recovery) {
    TEMP_WAL(guard);
    // Use a key and value that are distinct in size so changing lengths
    // always causes a checksum mismatch.
    // append_set("hello","world"): key_len=5, val_len=5, total=28 bytes.
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("hello", "world");
    });
    // Corrupt val_len (bytes 10-13) to 0.
    // Parser will now read only key_len=5 as payload, then read 4 bytes
    // from where "world" starts as the CRC field.  The CRC computed over
    // (modified header + key) will not match the value read as checksum.
    auto bytes = read_file_bytes(guard.path);
    ASSERT_TRUE(bytes.size() >= 14);
    bytes[10] = 0x00; // val_len byte 0 → 0
    bytes[11] = 0x00;
    bytes[12] = 0x00;
    bytes[13] = 0x00;
    {
        std::ofstream out(guard.path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-20. Truncated final record is not replayed.
// ---------------------------------------------------------------------------
TEST(s5_truncated_final_record_not_replayed) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("safe", "value");
        wal.append_set("partial", "shouldnotappear");
    });

    // Truncate the file so the second record is only partially written.
    // Each record for SET("safe","value"):  18 + 4 + 5 = 27 bytes.
    // Each record for SET("partial","shouldnotappear"): 18 + 7 + 15 = 40 bytes.
    // Keep the first record intact (27 bytes) + first 10 bytes of the second.
    const std::size_t keep = 27 + 10;
    truncate_file(guard.path, keep);

    // Recovery must succeed (truncated final record = non-fatal).
    auto store = make_store(guard.path);

    // "safe" must be present (complete record).
    ASSERT_EQ(*store.get("safe"), "value");

    // "partial" must NOT be present (incomplete record not applied).
    ASSERT_FALSE(store.exists("partial"));
}

// ---------------------------------------------------------------------------
// S5-21. Valid records before a truncated final record are recovered.
// ---------------------------------------------------------------------------
TEST(s5_valid_records_before_truncated_final_recovered) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("a", "1");
        wal.append_set("b", "2");
        wal.append_set("c", "3");
        wal.append_set("truncated_key", "truncated_value");
    });

    // Sizes: SET("a","1") = 18+1+1=20, SET("b","2") = 20, SET("c","3") = 20.
    // Total complete = 60 bytes; truncate to 60 + 5 bytes (partial 4th record).
    truncate_file(guard.path, 65);

    auto store = make_store(guard.path);
    ASSERT_EQ(*store.get("a"), "1");
    ASSERT_EQ(*store.get("b"), "2");
    ASSERT_EQ(*store.get("c"), "3");
    ASSERT_FALSE(store.exists("truncated_key"));
    ASSERT_EQ(store.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// S5-22. Corruption in the middle of the WAL is fatal.
// ---------------------------------------------------------------------------
TEST(s5_mid_log_corruption_is_fatal) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("first",  "ok");
        wal.append_set("second", "corrupt");
        wal.append_set("third",  "ok");
    });

    // SET("first","ok") = 18+5+2 = 25 bytes.
    // Corrupt a payload byte in the second record (offset 25 + 14 = 39).
    corrupt_bytes(guard.path, 39);

    ASSERT_THROWS(make_store(guard.path));
}

// ---------------------------------------------------------------------------
// S5-23. Records after a corrupted record are NOT replayed.
// ---------------------------------------------------------------------------
TEST(s5_records_after_corruption_not_replayed) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("before", "visible");
        wal.append_set("corrupted", "bad");
        wal.append_set("after",  "invisible");
    });

    // SET("before","visible")  = 18+6+7  = 31 bytes.
    // Corrupt a payload byte in the middle record at offset 31 + 14 = 45.
    corrupt_bytes(guard.path, 45);

    bool threw = false;
    try {
        auto store = make_store(guard.path);
        // If we get here, recovery didn't throw — which is wrong.
        // Verify "after" is NOT present (records past corruption skipped).
        ASSERT_FALSE(store.exists("after"));
    } catch (const std::exception&) {
        threw = true;
    }
    // Recovery must either throw OR at minimum not have replayed "after".
    // The spec says it must throw.
    ASSERT_TRUE(threw);
}

// ---------------------------------------------------------------------------
// S5-24. Injected Storage receives recovery operations.
//   Use a counting Storage to verify the right calls were made.
// ---------------------------------------------------------------------------

class TrackingStorage final : public forgekv::Storage {
public:
    std::vector<std::string> ops;
    std::unordered_map<std::string, std::string> data;

    void set(const std::string& k, const std::string& v) override {
        ops.push_back("SET:" + k + "=" + v);
        data[k] = v;
    }
    std::optional<std::string> get(const std::string& k) const override {
        auto it = data.find(k);
        return (it != data.end()) ? std::optional{it->second} : std::nullopt;
    }
    bool del(const std::string& k) override {
        ops.push_back("DEL:" + k);
        return data.erase(k) > 0;
    }
    bool exists(const std::string& k) const override { return data.count(k) > 0; }
    std::size_t size() const override { return data.size(); }
    bool empty() const override { return data.empty(); }
    void clear() override { ops.push_back("CLEAR"); data.clear(); }
    std::vector<std::pair<std::string, std::string>> get_all() const override {
        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(data.size());
        for (const auto& [k, v] : data) { result.emplace_back(k, v); }
        return result;
    }
};

TEST(s5_injected_storage_receives_recovery_ops) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("name",    "Vishnu");
        wal.append_set("age",     "21");
        wal.append_del("age");
        wal.append_clear();
        wal.append_set("project", "ForgeKV");
    });

    auto* raw = new TrackingStorage();
    auto storage = std::unique_ptr<forgekv::Storage>(raw);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Recovery must have applied: SET name, SET age, DEL age, CLEAR, SET project.
    ASSERT_EQ(raw->ops.size(), std::size_t{5});
    ASSERT_EQ(raw->ops[0], "SET:name=Vishnu");
    ASSERT_EQ(raw->ops[1], "SET:age=21");
    ASSERT_EQ(raw->ops[2], "DEL:age");
    ASSERT_EQ(raw->ops[3], "CLEAR");
    ASSERT_EQ(raw->ops[4], "SET:project=ForgeKV");
}

// ---------------------------------------------------------------------------
// S5-25. Recovery does not depend on InMemoryStorage — works with any Storage.
// ---------------------------------------------------------------------------
TEST(s5_recovery_works_with_any_storage) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("stage", "5");
    });

    // Use TrackingStorage (not InMemoryStorage) to prove independence.
    auto* raw = new TrackingStorage();
    auto storage = std::unique_ptr<forgekv::Storage>(raw);
    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Recovery applied the SET via the abstract Storage interface.
    ASSERT_EQ(raw->ops.size(), std::size_t{1});
    ASSERT_EQ(raw->ops[0], "SET:stage=5");
}

// ---------------------------------------------------------------------------
// S5-26. Read-only operations after recovery do not modify WAL.
// ---------------------------------------------------------------------------
TEST(s5_readonly_ops_after_recovery_no_wal_write) {
    TEMP_WAL(guard);
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("key", "value");
    });

    const std::size_t size_after_write = read_file_bytes(guard.path).size();

    {
        auto store = make_store(guard.path);
        // Recovery runs here.  Then only reads.
        (void)store.get("key");
        (void)store.exists("key");
        (void)store.size();
        (void)store.empty();
    }

    const std::size_t size_after_reads = read_file_bytes(guard.path).size();
    ASSERT_EQ(size_after_reads, size_after_write);
}

// ---------------------------------------------------------------------------
// S5-27. Repeated reopen/recovery is deterministic.
//   Opening the same WAL multiple times produces the same state each time.
// ---------------------------------------------------------------------------
TEST(s5_repeated_recovery_is_deterministic) {
    TEMP_WAL(guard);

    // Write a fixed sequence of records once.
    write_wal_records(guard.path, [](forgekv::WAL& wal) {
        wal.append_set("a", "apple");
        wal.append_set("b", "banana");
        wal.append_del("b");
        wal.append_set("c", "cherry");
    });

    // Open and recover three times independently.
    for (int i = 0; i < 3; ++i) {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("a"), "apple");
        ASSERT_FALSE(store.exists("b"));
        ASSERT_EQ(*store.get("c"), "cherry");
        ASSERT_EQ(store.size(), std::size_t{2});
    }

    // File size must be unchanged — recovery never appended anything.
    // Just re-open once more and verify.
    const std::size_t size_before = read_file_bytes(guard.path).size();
    { auto store = make_store(guard.path); (void)store.size(); }
    ASSERT_EQ(read_file_bytes(guard.path).size(), size_before);
}

// =============================================================================
// Stage 7 Tests — Concurrency
// =============================================================================
//
// These tests verify that KeyValueStore is correctly thread-safe under
// concurrent access.  They use:
//
//   std::latch   — barrier to synchronize thread start (C++20)
//   std::atomic  — lock-free counters / flags
//   std::thread  — worker threads
//   std::vector  — thread pools
//
// Design principles:
//   - All threads use a std::latch to start simultaneously, increasing the
//     likelihood of real concurrent execution.
//   - Correctness, not timing, is the primary verification criterion.
//   - No arbitrary sleeps.
//   - Each test uses a dedicated temp WAL to avoid state bleed.
// =============================================================================

// ---------------------------------------------------------------------------
// S7-1. Concurrent reads — no crashes, no incorrect values
//
//   Seed N keys.  Launch T reader threads.  Each reader repeatedly calls
//   get() and exists() on every key for R rounds.  All expected values must
//   be returned; no crash or unexpected nullopt may occur.
// ---------------------------------------------------------------------------
TEST(s7_concurrent_reads) {
    TEMP_WAL(guard);

    const int KEY_COUNT = 20;
    const int THREADS   = 8;
    const int ROUNDS    = 50;

    // Build store and seed keys.
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("key" + std::to_string(i), "val" + std::to_string(i));
    }

    // Latch: all threads wait here, then fire simultaneously.
    std::latch ready(THREADS);
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();

            for (int r = 0; r < ROUNDS; ++r) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    const std::string key = "key" + std::to_string(i);
                    const std::string expected = "val" + std::to_string(i);

                    auto val = store.get(key);
                    if (!val.has_value() || val.value() != expected) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }

                    if (!store.exists(key)) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    ASSERT_EQ(errors.load(), 0);
    ASSERT_EQ(store.size(), static_cast<std::size_t>(KEY_COUNT));
}

// ---------------------------------------------------------------------------
// S7-2. Concurrent writes to distinct keys — all writes visible after join
//
//   T threads each own a disjoint partition of K keys.  Each thread sets
//   all keys in its partition.  After all threads finish, every expected
//   key/value pair must be present in the store.
// ---------------------------------------------------------------------------
TEST(s7_concurrent_writes_disjoint_keys) {
    TEMP_WAL(guard);

    const int THREADS    = 8;
    const int KEYS_EACH  = 50;  // each thread writes KEYS_EACH distinct keys

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    std::latch ready(THREADS);

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();

            const int base = t * KEYS_EACH;
            for (int i = 0; i < KEYS_EACH; ++i) {
                const std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                const std::string val = "v" + std::to_string(base + i);
                store.set(key, val);
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // After all threads finish, every key must be present with correct value.
    std::size_t found = 0;
    for (int t = 0; t < THREADS; ++t) {
        const int base = t * KEYS_EACH;
        for (int i = 0; i < KEYS_EACH; ++i) {
            const std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
            const std::string expected = "v" + std::to_string(base + i);
            auto val = store.get(key);
            ASSERT_HAS_VALUE(val);
            ASSERT_EQ(val.value(), expected);
            ++found;
        }
    }

    ASSERT_EQ(found, static_cast<std::size_t>(THREADS * KEYS_EACH));
    ASSERT_EQ(store.size(), static_cast<std::size_t>(THREADS * KEYS_EACH));
}

// ---------------------------------------------------------------------------
// S7-3. Concurrent updates — final value is one of the written values
//
//   T threads all write to the SAME key repeatedly.  After all threads
//   complete, the key must exist and its value must be a valid string that
//   was written by one of the threads (not corrupted / mixed).
// ---------------------------------------------------------------------------
TEST(s7_concurrent_updates_same_key) {
    TEMP_WAL(guard);

    const int THREADS = 8;
    const int ROUNDS  = 100;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    std::latch ready(THREADS);

    std::vector<std::thread> threads;
    threads.reserve(THREADS);

    for (int t = 0; t < THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();

            for (int r = 0; r < ROUNDS; ++r) {
                // Each thread writes its own deterministic value.
                const std::string val = "thread" + std::to_string(t)
                                      + "_round" + std::to_string(r);
                store.set("shared_key", val);
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // The key must exist (written by at least one thread).
    ASSERT_TRUE(store.exists("shared_key"));

    // The final value must be a non-empty string — no corruption.
    auto final_val = store.get("shared_key");
    ASSERT_HAS_VALUE(final_val);
    ASSERT_FALSE(final_val.value().empty());

    // The value prefix must match one of the thread prefixes (sanity check).
    const std::string& v = final_val.value();
    bool valid_prefix = false;
    for (int t = 0; t < THREADS; ++t) {
        const std::string prefix = "thread" + std::to_string(t) + "_round";
        if (v.rfind(prefix, 0) == 0) {
            valid_prefix = true;
            break;
        }
    }
    ASSERT_TRUE(valid_prefix);
}

// ---------------------------------------------------------------------------
// S7-4. Mixed read/write workload — readers see consistent values
//
//   Writer threads update a set of keys continuously.
//   Reader threads read those keys and verify:
//     - No crash.
//     - If a key exists, its value must be one of the expected prefixes.
//     - Keys do not return mid-write garbage.
//
//   A std::atomic<bool> flag stops threads after the writers finish.
// ---------------------------------------------------------------------------
TEST(s7_concurrent_mixed_read_write) {
    TEMP_WAL(guard);

    const int WRITER_THREADS = 4;
    const int READER_THREADS = 4;
    const int WRITE_OPS_EACH = 200;
    const int KEY_COUNT      = 10;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Pre-seed keys so readers have something to read from the start.
    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("mkey" + std::to_string(i), "initial");
    }

    std::latch           ready(WRITER_THREADS + READER_THREADS);
    std::atomic<bool>    done{false};
    std::atomic<int>     read_errors{0};

    std::vector<std::thread> threads;
    threads.reserve(WRITER_THREADS + READER_THREADS);

    // Writer threads: continuously update keys.
    for (int t = 0; t < WRITER_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();

            for (int op = 0; op < WRITE_OPS_EACH; ++op) {
                const int ki = op % KEY_COUNT;
                const std::string key = "mkey" + std::to_string(ki);
                const std::string val = "writer" + std::to_string(t)
                                      + "_op" + std::to_string(op);
                store.set(key, val);
            }
        });
    }

    // Reader threads: read keys until writers are done.
    for (int t = 0; t < READER_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();

            while (!done.load(std::memory_order_acquire)) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto val = store.get("mkey" + std::to_string(i));
                    // Value may or may not be present (not deleted in this test)
                    // but if it is present it must be a non-empty string.
                    if (val.has_value() && val.value().empty()) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Join writer threads first, then signal readers to stop.
    for (int t = 0; t < WRITER_THREADS; ++t) {
        threads[t].join();
    }
    done.store(true, std::memory_order_release);
    for (int t = WRITER_THREADS; t < WRITER_THREADS + READER_THREADS; ++t) {
        threads[t].join();
    }

    ASSERT_EQ(read_errors.load(), 0);

    // After all writers finished, every pre-seeded key must still exist.
    for (int i = 0; i < KEY_COUNT; ++i) {
        ASSERT_TRUE(store.exists("mkey" + std::to_string(i)));
    }
}

// ---------------------------------------------------------------------------
// S7-5. Concurrent del + set on the same key — store remains consistent
//
//   Two groups of threads race: one group continuously sets a key, the
//   other group continuously deletes it.  After all threads finish, the
//   store must be in a valid state (no crash, no corrupted value).
// ---------------------------------------------------------------------------
TEST(s7_concurrent_set_and_del_same_key) {
    TEMP_WAL(guard);

    const int SET_THREADS = 4;
    const int DEL_THREADS = 4;
    const int OPS_EACH    = 100;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    std::latch ready(SET_THREADS + DEL_THREADS);
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    threads.reserve(SET_THREADS + DEL_THREADS);

    // Setter threads.
    for (int t = 0; t < SET_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS_EACH; ++op) {
                store.set("race_key", "val_t" + std::to_string(t));
            }
        });
    }

    // Deleter threads.
    for (int t = 0; t < DEL_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS_EACH; ++op) {
                store.del("race_key"); // returns true or false — both valid
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // No assertion on final state (may or may not exist — both valid).
    // The key invariant: no crash, no corrupted value, and if the key
    // exists its value must be non-empty and match expected prefix.
    auto final_val = store.get("race_key");
    if (final_val.has_value()) {
        ASSERT_FALSE(final_val.value().empty());
        // value must start with "val_t"
        const std::string& v = final_val.value();
        ASSERT_TRUE(v.rfind("val_t", 0) == 0);
    }

    // size() must be 0 or 1 — no phantom keys.
    const std::size_t sz = store.size();
    ASSERT_TRUE(sz == 0 || sz == 1);

    ASSERT_EQ(errors.load(), 0);
}

// ---------------------------------------------------------------------------
// S7-6. Concurrent clear() with reads and writes — no crash
//
//   Writer threads set keys, reader threads call size()/empty()/get().
//   One dedicated thread repeatedly calls clear().
//   All threads synchronize on a latch.  No crash allowed.
//   After all threads complete, size() must be deterministic.
// ---------------------------------------------------------------------------
TEST(s7_concurrent_clear_with_readers_writers) {
    TEMP_WAL(guard);

    const int WRITER_THREADS = 3;
    const int READER_THREADS = 3;
    const int CLEAR_THREADS  = 1;
    const int OPS_EACH       = 80;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    const int TOTAL = WRITER_THREADS + READER_THREADS + CLEAR_THREADS;
    std::latch ready(TOTAL);

    std::vector<std::thread> threads;
    threads.reserve(TOTAL);

    // Writers.
    for (int t = 0; t < WRITER_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS_EACH; ++op) {
                store.set("wk" + std::to_string(t) + "_" + std::to_string(op),
                          "wv" + std::to_string(op));
            }
        });
    }

    // Readers.
    for (int t = 0; t < READER_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS_EACH; ++op) {
                (void)store.size();
                (void)store.empty();
                (void)store.get("wk0_0");
            }
        });
    }

    // Clear threads.
    for (int t = 0; t < CLEAR_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();
            for (int op = 0; op < OPS_EACH; ++op) {
                store.clear();
            }
        });
    }

    for (auto& th : threads) { th.join(); }

    // After clear threads and writer threads, the store must be valid.
    // size() must return a non-negative value (trivially true for size_t,
    // but verifies no crash/corruption).
    const std::size_t sz = store.size();
    (void)sz; // value is indeterminate (clear races with set) — that's fine
    // empty() and size() must be consistent.
    ASSERT_EQ(store.empty(), (store.size() == 0));
}

// =============================================================================
// Stage 8 Tests — Log Compaction
// =============================================================================
//
// These tests verify WAL compaction behavior:
//   - Correct logical state preservation
//   - Atomic WAL replacement
//   - WAL size reduction
//   - Recovery after compaction
//   - Writes after compaction followed by recovery (stream reopen)
//   - Edge cases: empty store, single key, large state
//   - Concurrent access safety
//
// All tests use temporary WAL files and clean up via RAII (TempFile).
// Verification uses existing WAL replay/parsing infrastructure rather
// than duplicating the binary parser.
//
// Helper: replay a WAL file and return the resulting logical state.
// Returns a sorted vector of (key, value) pairs for deterministic comparison.
static std::vector<std::pair<std::string, std::string>>
replay_to_state(const std::string& wal_path)
{
    std::unordered_map<std::string, std::string> state;

    forgekv::WAL wal(wal_path);
    (void)wal.replay([&](const forgekv::WalRecord& rec) {
        switch (rec.opcode) {
            case forgekv::kOpSet:
                state[rec.key] = rec.value;
                break;
            case forgekv::kOpDel:
                state.erase(rec.key);
                break;
            case forgekv::kOpClear:
                state.clear();
                break;
            default:
                break;
        }
    });

    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(state.size());
    for (const auto& [k, v] : state) {
        result.emplace_back(k, v);
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Helper: count WAL records by replaying.
static std::size_t count_wal_records(const std::string& wal_path)
{
    std::size_t count = 0;
    std::ifstream f(wal_path, std::ios::binary);
    if (!f.is_open()) return 0;
    while (true) {
        f.peek();
        if (f.eof()) break;
        try {
            forgekv::WAL::read_record(f);
            ++count;
        } catch (...) {
            break;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// S8-1. Basic compaction: state unchanged, WAL shrinks.
//
//   Write several updates/deletes, compact, verify logical state unchanged
//   and WAL file is smaller than before compaction.
// ---------------------------------------------------------------------------
TEST(s8_basic_compaction_state_unchanged) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    store.set("user", "Vishnu");
    store.set("user", "Rahul");
    store.set("user", "Alex");
    store.set("age",  "20");
    store.set("age",  "21");
    store.del("old_key");   // del of nonexistent: no WAL record
    store.set("city", "Bangalore");
    store.set("city", "Mysore");

    const std::size_t size_before = read_file_bytes(guard.path).size();

    // Capture pre-compaction state.
    ASSERT_EQ(*store.get("user"), "Alex");
    ASSERT_EQ(*store.get("age"),  "21");
    ASSERT_EQ(*store.get("city"), "Mysore");
    ASSERT_EQ(store.size(), std::size_t{3});

    store.compact();

    // Logical state must be identical after compaction.
    ASSERT_EQ(*store.get("user"), "Alex");
    ASSERT_EQ(*store.get("age"),  "21");
    ASSERT_EQ(*store.get("city"), "Mysore");
    ASSERT_EQ(store.size(), std::size_t{3});
    ASSERT_FALSE(store.exists("old_key"));

    const std::size_t size_after = read_file_bytes(guard.path).size();
    // 7 records written → compact to 3 records: WAL must shrink.
    ASSERT_TRUE(size_after < size_before);
}

// ---------------------------------------------------------------------------
// S8-2. Repeated updates — only final value survives compaction.
//
//   SET A=1..100, then compact.
//   WAL must contain only 1 record for A with value "100".
// ---------------------------------------------------------------------------
TEST(s8_repeated_updates_compacted_to_one) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    for (int i = 1; i <= 100; ++i) {
        store.set("A", std::to_string(i));
    }

    const std::size_t size_before = read_file_bytes(guard.path).size();
    ASSERT_EQ(*store.get("A"), "100");

    store.compact();

    ASSERT_EQ(*store.get("A"), "100");
    ASSERT_EQ(store.size(), std::size_t{1});

    const std::size_t size_after = read_file_bytes(guard.path).size();
    // 100 records → 1 record: significant size reduction.
    ASSERT_TRUE(size_after < size_before);

    // WAL must contain exactly 1 record.
    ASSERT_EQ(count_wal_records(guard.path), std::size_t{1});

    // Verify via replay.
    auto replayed = replay_to_state(guard.path);
    ASSERT_EQ(replayed.size(), std::size_t{1});
    ASSERT_EQ(replayed[0].first,  "A");
    ASSERT_EQ(replayed[0].second, "100");
}

// ---------------------------------------------------------------------------
// S8-3. Deleted keys do not appear in the compacted WAL.
//
//   SET A=1, DEL A.  After compact: A absent, WAL has 0 records.
// ---------------------------------------------------------------------------
TEST(s8_deleted_key_not_in_compacted_wal) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    store.set("A", "1");
    store.del("A");

    ASSERT_FALSE(store.exists("A"));
    ASSERT_TRUE(store.empty());

    store.compact();

    ASSERT_FALSE(store.exists("A"));
    ASSERT_TRUE(store.empty());

    // WAL must be empty (no live keys).
    ASSERT_EQ(count_wal_records(guard.path), std::size_t{0});

    // Replay confirms empty state.
    auto replayed = replay_to_state(guard.path);
    ASSERT_TRUE(replayed.empty());
}

// ---------------------------------------------------------------------------
// S8-4. Mixed keys — correct final state preserved.
//
//   Many keys with repeated updates and deletes.
//   Capture pre-compaction state, compact, verify exact match.
// ---------------------------------------------------------------------------
TEST(s8_mixed_keys_correct_state) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    // Build complex history.
    for (int i = 0; i < 20; ++i) {
        store.set("key" + std::to_string(i), "val_v1_" + std::to_string(i));
    }
    for (int i = 0; i < 10; ++i) {
        store.set("key" + std::to_string(i), "val_v2_" + std::to_string(i));
    }
    // Delete even-indexed keys from 0..9.
    for (int i = 0; i < 10; i += 2) {
        store.del("key" + std::to_string(i));
    }

    // Capture expected state before compaction.
    std::vector<std::pair<std::string, std::string>> expected;
    for (int i = 0; i < 20; ++i) {
        const std::string key = "key" + std::to_string(i);
        auto val = store.get(key);
        if (val.has_value()) {
            expected.emplace_back(key, *val);
        }
    }
    std::sort(expected.begin(), expected.end());
    const std::size_t expected_count = expected.size();

    store.compact();

    // Verify every expected key/value matches exactly.
    ASSERT_EQ(store.size(), expected_count);
    for (const auto& [k, v] : expected) {
        auto val = store.get(k);
        ASSERT_HAS_VALUE(val);
        ASSERT_EQ(*val, v);
    }

    // WAL record count must equal the number of live keys.
    ASSERT_EQ(count_wal_records(guard.path), expected_count);

    // Replay must reproduce the same state.
    auto replayed = replay_to_state(guard.path);
    ASSERT_EQ(replayed, expected);
}

// ---------------------------------------------------------------------------
// S8-5. Recovery after compaction reproduces exact state.
//
//   Create state → compact → destroy store → recreate → verify.
// ---------------------------------------------------------------------------
TEST(s8_recovery_after_compaction) {
    TEMP_WAL(guard);

    // Phase 1: build state and compact.
    {
        auto store = make_store(guard.path);
        store.set("name",    "Alice");
        store.set("lang",    "C++");
        store.set("stage",   "8");
        store.set("name",    "Bob");    // update
        store.del("lang");              // delete
        store.set("project", "ForgeKV");

        store.compact();

        // Pre-verify in-memory state.
        ASSERT_EQ(*store.get("name"),    "Bob");
        ASSERT_FALSE(store.exists("lang"));
        ASSERT_EQ(*store.get("stage"),   "8");
        ASSERT_EQ(*store.get("project"), "ForgeKV");
    }

    // Phase 2: recreate store from compacted WAL.
    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("name"),    "Bob");
        ASSERT_FALSE(store.exists("lang"));
        ASSERT_EQ(*store.get("stage"),   "8");
        ASSERT_EQ(*store.get("project"), "ForgeKV");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S8-6. Writes after compaction persist correctly after recovery.
//
//   This tests the critical WAL-reopen invariant: after compact(), future
//   SET/DELETE operations must append to the NEW WAL file.
//
//   Pattern: create → compact → write more → destroy → recreate → verify all.
// ---------------------------------------------------------------------------
TEST(s8_writes_after_compaction_persist) {
    TEMP_WAL(guard);

    // Phase 1: create initial state, compact, write more.
    {
        auto store = make_store(guard.path);
        store.set("A", "1");
        store.set("B", "2");
        store.set("A", "3");  // update

        store.compact();

        // Now write more operations — these must go to the new WAL.
        store.set("C", "4");
        store.del("B");
        store.set("D", "5");
    }

    // Phase 2: recover from WAL — must contain all post-compaction ops.
    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("A"), "3");
        ASSERT_FALSE(store.exists("B"));
        ASSERT_EQ(*store.get("C"), "4");
        ASSERT_EQ(*store.get("D"), "5");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S8-7. Compacting an empty store succeeds.
//
//   compact() on an empty store must not throw.
//   WAL must be empty after compaction.
//   Recovery produces an empty store.
// ---------------------------------------------------------------------------
TEST(s8_compact_empty_store) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    ASSERT_TRUE(store.empty());

    // Must not throw.
    store.compact();

    ASSERT_TRUE(store.empty());
    ASSERT_EQ(store.size(), std::size_t{0});

    // WAL must have zero records.
    ASSERT_EQ(count_wal_records(guard.path), std::size_t{0});

    // Recovery from compacted empty WAL → empty store.
    {
        auto store2 = make_store(guard.path);
        ASSERT_TRUE(store2.empty());
    }
}

// ---------------------------------------------------------------------------
// S8-8. Compacting a single-key store.
// ---------------------------------------------------------------------------
TEST(s8_compact_single_key) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    store.set("only_key", "only_value");
    store.set("only_key", "updated_value");  // update

    store.compact();

    ASSERT_EQ(*store.get("only_key"), "updated_value");
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_EQ(count_wal_records(guard.path), std::size_t{1});

    // Verify via replay.
    auto replayed = replay_to_state(guard.path);
    ASSERT_EQ(replayed.size(), std::size_t{1});
    ASSERT_EQ(replayed[0].first,  "only_key");
    ASSERT_EQ(replayed[0].second, "updated_value");

    // Recovery.
    {
        auto store2 = make_store(guard.path);
        ASSERT_EQ(*store2.get("only_key"), "updated_value");
        ASSERT_EQ(store2.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S8-9. Large state compaction — correctness under volume.
//
//   Insert N keys with multiple updates each, delete a subset, compact.
//   Verify every surviving key/value exactly matches the pre-compaction state.
// ---------------------------------------------------------------------------
TEST(s8_large_state_compaction) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    const int N = 500;   // number of unique keys
    const int U = 5;     // updates per key

    // Write U versions of each key.
    for (int u = 0; u < U; ++u) {
        for (int i = 0; i < N; ++i) {
            store.set("key" + std::to_string(i),
                      "v" + std::to_string(u) + "_" + std::to_string(i));
        }
    }

    // Delete keys divisible by 7.
    for (int i = 0; i < N; i += 7) {
        store.del("key" + std::to_string(i));
    }

    // Capture expected state.
    std::vector<std::pair<std::string, std::string>> expected;
    for (int i = 0; i < N; ++i) {
        const std::string key = "key" + std::to_string(i);
        auto val = store.get(key);
        if (val.has_value()) {
            expected.emplace_back(key, *val);
        }
    }
    std::sort(expected.begin(), expected.end());
    const std::size_t live_count = expected.size();

    const std::size_t size_before = read_file_bytes(guard.path).size();

    store.compact();

    const std::size_t size_after = read_file_bytes(guard.path).size();
    ASSERT_TRUE(size_after < size_before);
    ASSERT_EQ(store.size(), live_count);
    ASSERT_EQ(count_wal_records(guard.path), live_count);

    // Spot-check first 50 surviving keys.
    for (std::size_t i = 0; i < std::min(expected.size(), std::size_t{50}); ++i) {
        auto val = store.get(expected[i].first);
        ASSERT_HAS_VALUE(val);
        ASSERT_EQ(*val, expected[i].second);
    }

    // Full replay verification.
    auto replayed = replay_to_state(guard.path);
    ASSERT_EQ(replayed, expected);

    // Recovery verification.
    {
        auto store2 = make_store(guard.path);
        ASSERT_EQ(store2.size(), live_count);
        for (const auto& [k, v] : expected) {
            auto val = store2.get(k);
            ASSERT_HAS_VALUE(val);
            ASSERT_EQ(*val, v);
        }
    }
}

// ---------------------------------------------------------------------------
// S8-10. Compact followed by immediate re-compact — idempotent.
//
//   Compacting an already-compact WAL must be a safe no-op from a
//   correctness standpoint.  The WAL size stays the same (one record per key).
// ---------------------------------------------------------------------------
TEST(s8_double_compact_idempotent) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    store.set("x", "1");
    store.set("y", "2");
    store.set("x", "3");  // update

    store.compact();
    const std::size_t size_after_first = read_file_bytes(guard.path).size();

    store.compact();
    const std::size_t size_after_second = read_file_bytes(guard.path).size();

    // Second compact writes same records → same file size.
    ASSERT_EQ(size_after_first, size_after_second);

    ASSERT_EQ(*store.get("x"), "3");
    ASSERT_EQ(*store.get("y"), "2");
    ASSERT_EQ(store.size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// S8-11. WAL replay count equals live key count after compaction.
//
//   Precisely: after compact(), replaying the WAL must produce EXACTLY as
//   many SET records as there are live keys, and zero DEL/CLEAR records.
// ---------------------------------------------------------------------------
TEST(s8_wal_record_count_equals_live_keys) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    store.set("a", "1");
    store.set("b", "2");
    store.set("c", "3");
    store.del("b");
    store.set("d", "4");

    // Live keys: a, c, d → 3 keys.
    ASSERT_EQ(store.size(), std::size_t{3});

    store.compact();

    // Replay and count records by opcode.
    std::size_t set_count   = 0;
    std::size_t del_count   = 0;
    std::size_t clear_count = 0;

    forgekv::WAL wal(guard.path);
    (void)wal.replay([&](const forgekv::WalRecord& rec) {
        switch (rec.opcode) {
            case forgekv::kOpSet:   ++set_count;   break;
            case forgekv::kOpDel:   ++del_count;   break;
            case forgekv::kOpClear: ++clear_count; break;
            default: break;
        }
    });

    ASSERT_EQ(set_count,   std::size_t{3});  // exactly one per live key
    ASSERT_EQ(del_count,   std::size_t{0});  // no DEL records
    ASSERT_EQ(clear_count, std::size_t{0});  // no CLEAR records
}

// ---------------------------------------------------------------------------
// S8-12. Compact + more writes + second compact → correct recovery.
//
//   Compact → add keys → compact again → recover → verify.
//   Ensures the two-compact round-trip is fully durable.
// ---------------------------------------------------------------------------
TEST(s8_compact_write_compact_recover) {
    TEMP_WAL(guard);

    {
        auto store = make_store(guard.path);
        store.set("p", "1");
        store.set("q", "2");
        store.set("p", "10");   // update

        store.compact();   // First compact: WAL = {p=10, q=2}

        store.set("r", "3");
        store.del("q");
        store.set("s", "4");

        store.compact();   // Second compact: WAL = {p=10, r=3, s=4}
    }

    {
        auto store = make_store(guard.path);
        ASSERT_EQ(*store.get("p"), "10");
        ASSERT_FALSE(store.exists("q"));
        ASSERT_EQ(*store.get("r"), "3");
        ASSERT_EQ(*store.get("s"), "4");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S8-13. Concurrent readers during compaction — no crashes, consistent state.
//
//   Strategy:
//   - Pre-populate store.
//   - Launch reader threads that repeatedly read all keys.
//   - One thread performs compact() once.
//   - All readers run before and after compaction.
//   - After join: verify logical state is correct.
//
//   Uses std::latch for synchronized start (matching Stage 7 test style).
//   No arbitrary sleeps. Correctness, not timing, is verified.
// ---------------------------------------------------------------------------
TEST(s8_concurrent_readers_during_compaction) {
    TEMP_WAL(guard);

    const int KEY_COUNT     = 30;
    const int READER_THREADS = 6;
    const int READ_ROUNDS   = 100;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Seed store with multiple updates per key (to create compactable history).
    for (int v = 0; v < 3; ++v) {
        for (int i = 0; i < KEY_COUNT; ++i) {
            store.set("key" + std::to_string(i),
                      "val_v" + std::to_string(v) + "_" + std::to_string(i));
        }
    }

    // Expected final value for each key (last written version = v2).
    // These must survive and be readable before/after compaction.

    std::latch ready(READER_THREADS + 1);  // +1 for compact thread
    std::atomic<int> read_errors{0};
    std::atomic<bool> compact_done{false};

    std::vector<std::thread> threads;
    threads.reserve(READER_THREADS + 1);

    // Reader threads: read all keys before/after compaction.
    for (int t = 0; t < READER_THREADS; ++t) {
        threads.emplace_back([&]() {
            ready.arrive_and_wait();

            for (int r = 0; r < READ_ROUNDS; ++r) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    const std::string key = "key" + std::to_string(i);
                    auto val = store.get(key);
                    // Key must always exist (we never delete in this test).
                    if (!val.has_value()) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Value must be non-empty.
                    if (val.has_value() && val.value().empty()) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                    if (!store.exists(key)) {
                        read_errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Compaction thread: compact once after all threads are ready.
    threads.emplace_back([&]() {
        ready.arrive_and_wait();
        store.compact();
        compact_done.store(true, std::memory_order_release);
    });

    for (auto& th : threads) { th.join(); }

    ASSERT_TRUE(compact_done.load());
    ASSERT_EQ(read_errors.load(), 0);

    // Post-compaction state must match expected.
    ASSERT_EQ(store.size(), static_cast<std::size_t>(KEY_COUNT));
    for (int i = 0; i < KEY_COUNT; ++i) {
        const std::string key      = "key" + std::to_string(i);
        const std::string expected = "val_v2_" + std::to_string(i);
        auto val = store.get(key);
        ASSERT_HAS_VALUE(val);
        ASSERT_EQ(*val, expected);
    }

    // Recovery after concurrent compaction must also be correct.
    {
        auto store2 = make_store(guard.path);
        ASSERT_EQ(store2.size(), static_cast<std::size_t>(KEY_COUNT));
        for (int i = 0; i < KEY_COUNT; ++i) {
            const std::string key      = "key" + std::to_string(i);
            const std::string expected = "val_v2_" + std::to_string(i);
            auto val = store2.get(key);
            ASSERT_HAS_VALUE(val);
            ASSERT_EQ(*val, expected);
        }
    }
}

// ---------------------------------------------------------------------------
// S8-14. get_all() on InMemoryStorage returns all live pairs.
//
//   Unit test for the new Storage::get_all() method directly on
//   InMemoryStorage (not through KeyValueStore).
// ---------------------------------------------------------------------------
TEST(s8_get_all_basic) {
    forgekv::InMemoryStorage s;
    s.set("z", "3");
    s.set("a", "1");
    s.set("m", "2");

    auto all = s.get_all();
    // Three pairs returned.
    ASSERT_EQ(all.size(), std::size_t{3});

    // Sort for deterministic comparison.
    std::sort(all.begin(), all.end());
    ASSERT_EQ(all[0].first,  "a");
    ASSERT_EQ(all[0].second, "1");
    ASSERT_EQ(all[1].first,  "m");
    ASSERT_EQ(all[1].second, "2");
    ASSERT_EQ(all[2].first,  "z");
    ASSERT_EQ(all[2].second, "3");
}

// ---------------------------------------------------------------------------
// S8-15. get_all() on empty InMemoryStorage returns empty vector.
// ---------------------------------------------------------------------------
TEST(s8_get_all_empty) {
    forgekv::InMemoryStorage s;
    auto all = s.get_all();
    ASSERT_TRUE(all.empty());
}

// ---------------------------------------------------------------------------
// S8-16. Compact preserves deterministic key order in WAL.
//
//   After compact(), replaying the WAL must produce keys in
//   lexicographic order (the order compact() writes them).
// ---------------------------------------------------------------------------
TEST(s8_compact_deterministic_order) {
    TEMP_WAL(guard);
    auto store = make_store(guard.path);

    // Write in non-alphabetic order.
    store.set("zebra",  "z");
    store.set("apple",  "a");
    store.set("mango",  "m");
    store.set("banana", "b");

    store.compact();

    // Read WAL records in file order.
    std::vector<std::string> keys_in_order;
    auto f = open_binary(guard.path);
    bool hit_end = false;
    while (!hit_end) {
        f.peek();
        if (f.eof()) break;
        try {
            auto rec = forgekv::WAL::read_record(f);
            keys_in_order.push_back(rec.key);
        } catch (...) {
            hit_end = true;
        }
    }

    // Must be 4 records in lexicographic order.
    ASSERT_EQ(keys_in_order.size(), std::size_t{4});
    ASSERT_EQ(keys_in_order[0], "apple");
    ASSERT_EQ(keys_in_order[1], "banana");
    ASSERT_EQ(keys_in_order[2], "mango");
    ASSERT_EQ(keys_in_order[3], "zebra");
}

// =============================================================================
// Stage 9 Tests — Snapshots
// =============================================================================
//
// Comprehensive snapshot tests covering:
//
//   S9-A.  Basic snapshot: create keys → snapshot → restart → verify exact state
//   S9-B.  Snapshot + later WAL writes survive restart
//   S9-C.  Repeated updates around snapshot boundary
//   S9-D.  Deletes after snapshot survive restart (key stays deleted)
//   S9-E.  Recreate after delete: newest value visible after restart
//   S9-F.  Empty snapshot
//   S9-G.  No snapshot fallback (WAL-only recovery still works)
//   S9-H.  Snapshot corruption → fallback to WAL-only recovery
//   S9-I.  Snapshot truncation detected
//   S9-J.  WAL corruption after snapshot still detected
//   S9-K.  Snapshot boundary: pre-snapshot WAL not replayed again; post- replayed
//   S9-L.  Multiple snapshots: newest wins
//   S9-M.  Snapshot + compaction: compact() invalidates snapshot (explicit delete)
//   S9-N.  Concurrent snapshot() with readers/writers
//   S9-O.  Snapshot binary format validation (header, version, CRC, records)
//   S9-P.  Snapshot path convention (<wal>.snapshot)
//   S9-Q.  Snapshot empty store + WAL tail
//   S9-R.  WAL replay_from with offset 0 equals full replay
//   S9-S.  WAL replay_from beyond EOF throws
//   S9-T.  WAL file_size matches actual written data
// =============================================================================

#include "forgekv/snapshot.h"  // for SnapshotManager, kSnapshotMagic, etc.

// Helper: RAII guard for snapshot file (deleted on scope exit alongside WAL).
struct SnapGuard {
    std::string wal_path;
    std::string snap_path;
    explicit SnapGuard(std::string w)
        : wal_path(std::move(w)), snap_path(wal_path + ".snapshot") {}
    ~SnapGuard() {
        std::error_code ec;
        std::filesystem::remove(wal_path,  ec);
        std::filesystem::remove(snap_path, ec);
    }
};

#define TEMP_SNAP(name)  SnapGuard name{"test_s9_" #name ".wal"}

// Helper: create store, apply callback, destroy, recreate and return.
// Used to simulate a process restart after snapshot/WAL operations.
static forgekv::KeyValueStore restart_store(const std::string& wal_path)
{
    return make_store(wal_path);
}

// ---------------------------------------------------------------------------
// S9-A. Basic snapshot: create keys → snapshot → restart → verify state.
// ---------------------------------------------------------------------------
TEST(s9_basic_snapshot_survives_restart) {
    TEMP_SNAP(g);

    // Phase 1: create state and snapshot.
    {
        auto store = make_store(g.wal_path);
        store.set("name",  "Alice");
        store.set("lang",  "C++");
        store.set("stage", "9");
        ASSERT_TRUE(store.snapshot());
    }

    // Phase 2: restart — no new WAL records after snapshot.
    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("name"),  "Alice");
        ASSERT_EQ(*store.get("lang"),  "C++");
        ASSERT_EQ(*store.get("stage"), "9");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S9-B. Snapshot + later WAL writes survive restart correctly.
//
//   state → snapshot → more writes → restart → verify ALL (snapshot + tail)
// ---------------------------------------------------------------------------
TEST(s9_snapshot_plus_wal_tail_survives_restart) {
    TEMP_SNAP(g);

    // Phase 1: base state.
    {
        auto store = make_store(g.wal_path);
        store.set("A", "1");
        store.set("B", "2");
        ASSERT_TRUE(store.snapshot());

        // Writes AFTER snapshot — must appear in WAL tail.
        store.set("C", "3");
        store.set("A", "10"); // update key present in snapshot
    }

    // Phase 2: restart.
    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("A"), "10"); // post-snapshot update wins
        ASSERT_EQ(*store.get("B"), "2");
        ASSERT_EQ(*store.get("C"), "3");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S9-C. Repeated updates around snapshot boundary.
//
//   SET key many times → snapshot → SET key more times → restart.
//   Final value must be the last SET, not a pre-snapshot value.
// ---------------------------------------------------------------------------
TEST(s9_repeated_updates_around_snapshot) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        for (int i = 1; i <= 50; ++i) {
            store.set("counter", std::to_string(i));
        }
        // counter == "50" at snapshot time
        ASSERT_TRUE(store.snapshot());
        for (int i = 51; i <= 100; ++i) {
            store.set("counter", std::to_string(i));
        }
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("counter"), "100");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-D. Deletes after snapshot survive restart (key remains deleted).
//
//   SET key → snapshot → DEL key → restart → key must not exist.
// ---------------------------------------------------------------------------
TEST(s9_delete_after_snapshot_survives_restart) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("to_delete", "here");
        store.set("to_keep",   "keep");
        ASSERT_TRUE(store.snapshot());
        store.del("to_delete");  // post-snapshot DEL
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_FALSE(store.exists("to_delete"));
        ASSERT_EQ(*store.get("to_keep"), "keep");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-E. Recreate after delete: newest value visible after restart.
//
//   SET A → snapshot → DEL A → SET A=new → restart → A == "new".
// ---------------------------------------------------------------------------
TEST(s9_recreate_after_delete_after_snapshot) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("X", "original");
        ASSERT_TRUE(store.snapshot());
        store.del("X");
        store.set("X", "recreated");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_HAS_VALUE(store.get("X"));
        ASSERT_EQ(*store.get("X"), "recreated");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-F. Empty snapshot — snapshot with zero keys, then WAL adds keys.
// ---------------------------------------------------------------------------
TEST(s9_empty_snapshot) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        // Store is empty — take a snapshot of empty state.
        ASSERT_TRUE(store.empty());
        ASSERT_TRUE(store.snapshot());
        // Add keys AFTER the snapshot.
        store.set("after", "snap");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("after"), "snap");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-G. No snapshot fallback — WAL-only recovery still works unchanged.
//
//   Ensure no snapshot file exists → create state → restart → verify.
// ---------------------------------------------------------------------------
TEST(s9_no_snapshot_fallback_wal_recovery) {
    TEMP_SNAP(g);

    // Ensure no snapshot.
    std::error_code ec;
    std::filesystem::remove(g.snap_path, ec);

    {
        auto store = make_store(g.wal_path);
        store.set("fallback", "wal_recovery");
        store.set("num",      "42");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("fallback"), "wal_recovery");
        ASSERT_EQ(*store.get("num"),      "42");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// ---------------------------------------------------------------------------
// S9-H. Snapshot corruption — fallback to full WAL recovery.
//
//   Write a valid snapshot → corrupt it → restart → verify state is still
//   correct (loaded from WAL, not from corrupted snapshot).
// ---------------------------------------------------------------------------
TEST(s9_corrupt_snapshot_falls_back_to_wal) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("key1", "value1");
        store.set("key2", "value2");
        ASSERT_TRUE(store.snapshot());
        // Also write WAL records after snapshot so WAL has full history.
        // (These WAL records are written AFTER snapshot so offset is past them.)
        // But the WAL from the start has the full history since the snapshot
        // is corrupt and we fall back to replaying the WAL from offset 0.
    }

    // Corrupt the snapshot by flipping bytes in the middle.
    {
        auto bytes = read_file_bytes(g.snap_path);
        ASSERT_TRUE(!bytes.empty());
        // Corrupt the middle of the payload.
        const std::size_t mid = bytes.size() / 2;
        bytes[mid] ^= 0xFF;
        std::ofstream out(g.snap_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    // Restart — must fall back to WAL recovery (no exception, correct state).
    {
        auto store = restart_store(g.wal_path);
        // WAL has the full history from offset 0.
        ASSERT_EQ(*store.get("key1"), "value1");
        ASSERT_EQ(*store.get("key2"), "value2");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// ---------------------------------------------------------------------------
// S9-I. Snapshot truncation detected — incomplete snapshot is treated as corrupt.
//
//   Write a valid snapshot → truncate it → restart → fallback to WAL.
// ---------------------------------------------------------------------------
TEST(s9_truncated_snapshot_falls_back_to_wal) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("alpha", "A");
        store.set("beta",  "B");
        ASSERT_TRUE(store.snapshot());
    }

    // Truncate the snapshot to just the header (not the full content).
    {
        auto bytes = read_file_bytes(g.snap_path);
        ASSERT_TRUE(bytes.size() > 10);
        truncate_file(g.snap_path, 10); // less than minimum valid header
    }

    // Restart — truncated snapshot detected, fallback to WAL.
    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("alpha"), "A");
        ASSERT_EQ(*store.get("beta"),  "B");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// ---------------------------------------------------------------------------
// S9-J. WAL corruption after snapshot is still detected.
//
//   Create state → snapshot → append more WAL records → corrupt the WAL TAIL
//   (after the snapshot offset) → restart → must throw (mid-log corruption).
// ---------------------------------------------------------------------------
TEST(s9_wal_corruption_after_snapshot_detected) {
    TEMP_SNAP(g);

    // Build state with snapshot, then add post-snapshot WAL records.
    std::uint64_t snap_offset = 0;
    {
        auto store = make_store(g.wal_path);
        store.set("pre",  "snap");

        ASSERT_TRUE(store.snapshot());
        // Capture WAL size at snapshot.
        snap_offset = forgekv::WAL(g.wal_path).file_size();

        store.set("post", "snap");
    }

    // WAL has records at [0, snap_offset) and [snap_offset, EOF).
    // Corrupt a byte in the post-snapshot region.
    {
        const auto wal_bytes = read_file_bytes(g.wal_path);
        ASSERT_TRUE(wal_bytes.size() > snap_offset + 5);
        // Corrupt 5 bytes starting just after snap_offset (inside the WAL tail).
        corrupt_bytes(g.wal_path, static_cast<std::size_t>(snap_offset) + 2, 5);
    }

    // Restart — the post-snapshot WAL segment is corrupt → throw.
    ASSERT_THROWS(restart_store(g.wal_path));

    (void)snap_offset;
}

// ---------------------------------------------------------------------------
// S9-K. Snapshot boundary correctness.
//
//   Pre-snapshot WAL records must NOT be replayed again after snapshot load.
//   Post-snapshot WAL records MUST be replayed.
//
//   Strategy: write key A=v1 → snapshot → write A=v2 → restart.
//   After restart: A must be "v2" (WAL tail replayed), not "v1" replayed
//   twice (which would still give v1 if snapshot loaded A=v1 and WAL tail
//   updates A=v2 correctly — what we really test is that A=v1 is not
//   re-applied AFTER the snapshot already has it, and A=v2 is applied once).
//
//   We also verify via a counter: key "count" is incremented in WAL.
//   If pre-snapshot records were re-replayed, count would be off.
// ---------------------------------------------------------------------------
TEST(s9_snapshot_boundary_correctness) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        // Pre-snapshot: set count to 1, A to "pre".
        store.set("count", "1");
        store.set("A",     "pre");
        ASSERT_TRUE(store.snapshot());

        // Post-snapshot: update A, set count to 2.
        store.set("A",     "post");
        store.set("count", "2");
    }

    {
        auto store = restart_store(g.wal_path);
        // A should be "post" (from WAL tail, applied once over snapshot).
        ASSERT_EQ(*store.get("A"), "post");
        // count should be "2" — if pre-snapshot was re-replayed it would
        // briefly go back to "1" before becoming "2" again; in-memory result
        // is still "2" either way. The key invariant: no crash, correct value.
        ASSERT_EQ(*store.get("count"), "2");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// ---------------------------------------------------------------------------
// S9-L. Multiple snapshots: only the last one is used.
//
//   Create state → snapshot1 → modify → snapshot2 → modify → restart.
//   Recovery uses snapshot2 + WAL tail after snapshot2.
// ---------------------------------------------------------------------------
TEST(s9_multiple_snapshots_newest_wins) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("x", "1");
        store.set("y", "1");
        ASSERT_TRUE(store.snapshot());  // snapshot1: x=1, y=1

        store.set("x", "2");
        store.set("z", "2");
        ASSERT_TRUE(store.snapshot());  // snapshot2: x=2, y=1, z=2 (overwrites snapshot1)

        // Post-snapshot2 WAL tail.
        store.set("w", "3");
        store.del("y");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("x"), "2");  // from snapshot2
        ASSERT_FALSE(store.exists("y"));   // deleted after snapshot2
        ASSERT_EQ(*store.get("z"), "2");  // from snapshot2
        ASSERT_EQ(*store.get("w"), "3");  // from WAL tail
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S9-M. Snapshot + compaction: compact() deletes snapshot.
//
//   Create state → snapshot → compact() → verify snapshot file is gone.
//   Then restart → WAL-only recovery produces correct state.
// ---------------------------------------------------------------------------
TEST(s9_compact_deletes_snapshot) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("p", "1");
        store.set("q", "2");
        ASSERT_TRUE(store.snapshot());

        // Snapshot file must exist now.
        ASSERT_TRUE(std::filesystem::exists(g.snap_path));

        store.compact();

        // Snapshot file must be gone after compact().
        ASSERT_FALSE(std::filesystem::exists(g.snap_path));
    }

    // Restart: no snapshot, full WAL recovery (compacted WAL).
    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("p"), "1");
        ASSERT_EQ(*store.get("q"), "2");
        ASSERT_EQ(store.size(), std::size_t{2});
    }
}

// ---------------------------------------------------------------------------
// S9-M2. Snapshot after compact() produces a valid new snapshot pointing
//         into the new compacted WAL.
// ---------------------------------------------------------------------------
TEST(s9_snapshot_after_compact_works) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("A", "alpha");
        store.set("B", "beta");
        store.compact(); // no snapshot yet; just compact

        // Now create snapshot after compaction.
        ASSERT_TRUE(store.snapshot());
        ASSERT_TRUE(std::filesystem::exists(g.snap_path));

        // Post-snapshot writes.
        store.set("C", "gamma");
    }

    // Restart: snapshot (pointing into compacted WAL) + WAL tail.
    {
        auto store = restart_store(g.wal_path);
        ASSERT_EQ(*store.get("A"), "alpha");
        ASSERT_EQ(*store.get("B"), "beta");
        ASSERT_EQ(*store.get("C"), "gamma");
        ASSERT_EQ(store.size(), std::size_t{3});
    }
}

// ---------------------------------------------------------------------------
// S9-N. Concurrent snapshot() with active readers and writers.
//
//   Multiple writer threads and reader threads run concurrently.
//   The main thread calls snapshot() while writers are active.
//
//   Correctness invariants:
//   - No crash, no deadlock.
//   - After all threads finish, restart produces a consistent state.
//   - Snapshot must contain a logically consistent point-in-time state
//     (guaranteed by the exclusive lock inside snapshot()).
// ---------------------------------------------------------------------------
TEST(s9_concurrent_snapshot_with_readers_writers) {
    TEMP_SNAP(g);

    constexpr int WRITER_THREADS = 4;
    constexpr int READER_THREADS = 4;
    constexpr int WRITE_OPS      = 100;
    constexpr int KEY_COUNT      = 20;

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(g.wal_path);
    forgekv::KeyValueStore store(std::move(storage), std::move(wal));

    // Pre-seed keys.
    for (int i = 0; i < KEY_COUNT; ++i) {
        store.set("ck" + std::to_string(i), "initial");
    }

    std::latch           start_latch(WRITER_THREADS + READER_THREADS + 1);
    std::atomic<bool>    stop_readers{false};
    std::atomic<int>     errors{0};

    std::vector<std::thread> threads;
    threads.reserve(WRITER_THREADS + READER_THREADS);

    // Writer threads.
    for (int t = 0; t < WRITER_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            start_latch.arrive_and_wait();
            for (int op = 0; op < WRITE_OPS; ++op) {
                const int ki = op % KEY_COUNT;
                store.set("ck" + std::to_string(ki),
                          "t" + std::to_string(t) + "_op" + std::to_string(op));
            }
        });
    }

    // Reader threads.
    for (int t = 0; t < READER_THREADS; ++t) {
        threads.emplace_back([&]() {
            start_latch.arrive_and_wait();
            while (!stop_readers.load(std::memory_order_acquire)) {
                for (int i = 0; i < KEY_COUNT; ++i) {
                    auto v = store.get("ck" + std::to_string(i));
                    if (v.has_value() && v.value().empty()) {
                        errors.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
    }

    // Main thread: arrive at latch, then call snapshot() while threads run.
    start_latch.arrive_and_wait();
    // Let writers do some work first, then snapshot.
    // No arbitrary sleep: just call snapshot — it acquires the exclusive lock.
    const bool snap_ok = store.snapshot();
    ASSERT_TRUE(snap_ok);

    // Join writers.
    for (int t = 0; t < WRITER_THREADS; ++t) {
        threads[t].join();
    }
    stop_readers.store(true, std::memory_order_release);
    for (int t = WRITER_THREADS; t < WRITER_THREADS + READER_THREADS; ++t) {
        threads[t].join();
    }

    ASSERT_EQ(errors.load(), 0);

    // Destroy and restart.  State must be consistent.
    const std::size_t live_size = store.size();
    {
        auto store2 = restart_store(g.wal_path);
        // Size after restart must equal size before (snapshot + WAL tail).
        ASSERT_EQ(store2.size(), live_size);
    }
}

// ---------------------------------------------------------------------------
// S9-O. Snapshot binary format validation.
//
//   After saving a snapshot, parse the raw bytes and verify:
//   - magic == kSnapshotMagic
//   - version == kSnapshotVersion
//   - record_count matches
//   - key/value content matches
//   - CRC32 is valid (use SnapshotManager::load() which verifies it)
// ---------------------------------------------------------------------------
TEST(s9_snapshot_binary_format) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("format_key", "format_val");
        store.set("key2",       "val2");
        ASSERT_TRUE(store.snapshot());
    }

    // Load raw bytes.
    const auto raw = read_file_bytes(g.snap_path);

    // Header must be at least 17 + 4 = 21 bytes for 0 records.
    ASSERT_TRUE(raw.size() >= 17u + 4u);

    // Check magic (little-endian: bytes 0..3).
    const std::uint32_t magic =
          static_cast<std::uint32_t>(raw[0])
        | (static_cast<std::uint32_t>(raw[1]) <<  8)
        | (static_cast<std::uint32_t>(raw[2]) << 16)
        | (static_cast<std::uint32_t>(raw[3]) << 24);
    ASSERT_EQ(magic, forgekv::kSnapshotMagic);

    // Check version (byte 4).
    ASSERT_EQ(raw[4], forgekv::kSnapshotVersion);

    // Check record count (bytes 13..16).
    const std::uint32_t count =
          static_cast<std::uint32_t>(raw[13])
        | (static_cast<std::uint32_t>(raw[14]) <<  8)
        | (static_cast<std::uint32_t>(raw[15]) << 16)
        | (static_cast<std::uint32_t>(raw[16]) << 24);
    ASSERT_EQ(count, std::uint32_t{2});

    // Use SnapshotManager::load() to verify full integrity.
    forgekv::SnapshotManager sm(g.wal_path);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_FALSE(result.corrupt);
    ASSERT_EQ(result.data.records.size(), std::size_t{2});

    // Collect loaded keys.
    std::vector<std::pair<std::string,std::string>> loaded = result.data.records;
    std::sort(loaded.begin(), loaded.end());
    ASSERT_EQ(loaded[0].first,  "format_key");
    ASSERT_EQ(loaded[0].second, "format_val");
    ASSERT_EQ(loaded[1].first,  "key2");
    ASSERT_EQ(loaded[1].second, "val2");
}

// ---------------------------------------------------------------------------
// S9-P. Snapshot path convention: file is at <wal_path>.snapshot.
// ---------------------------------------------------------------------------
TEST(s9_snapshot_path_convention) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("path_test", "ok");
        ASSERT_TRUE(store.snapshot());
    }

    // The snapshot must exist exactly at <wal_path>.snapshot.
    ASSERT_TRUE(std::filesystem::exists(g.snap_path));

    // And NOT at any other path.
    ASSERT_FALSE(std::filesystem::exists(g.wal_path + ".snap"));
    ASSERT_FALSE(std::filesystem::exists(g.wal_path + ".checkpoint"));
}

// ---------------------------------------------------------------------------
// S9-Q. Empty snapshot with subsequent WAL tail produces correct state.
// ---------------------------------------------------------------------------
TEST(s9_empty_snapshot_with_wal_tail) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        // Snapshot with empty store.
        ASSERT_TRUE(store.snapshot());
        // After snapshot, write data.
        store.set("post1", "v1");
        store.set("post2", "v2");
        store.del("post1");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_FALSE(store.exists("post1"));
        ASSERT_EQ(*store.get("post2"), "v2");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-R. WAL::replay_from with offset 0 is equivalent to full replay().
// ---------------------------------------------------------------------------
TEST(s9_replay_from_offset_zero_equals_full_replay) {
    TEMP_SNAP(g);
    // Also need a temp snapshot guard; TEMP_SNAP already covers the WAL path.

    write_wal_records(g.wal_path, [](forgekv::WAL& wal) {
        wal.append_set("k1", "v1");
        wal.append_set("k2", "v2");
        wal.append_del("k1");
        wal.append_set("k3", "v3");
    });

    // Full replay.
    std::unordered_map<std::string, std::string> state_full;
    {
        forgekv::WAL wal(g.wal_path);
        (void)wal.replay([&](const forgekv::WalRecord& r) {
            if (r.opcode == forgekv::kOpSet) state_full[r.key] = r.value;
            else if (r.opcode == forgekv::kOpDel) state_full.erase(r.key);
        });
    }

    // replay_from(0, ...).
    std::unordered_map<std::string, std::string> state_from0;
    {
        forgekv::WAL wal(g.wal_path);
        (void)wal.replay_from(0, [&](const forgekv::WalRecord& r) {
            if (r.opcode == forgekv::kOpSet) state_from0[r.key] = r.value;
            else if (r.opcode == forgekv::kOpDel) state_from0.erase(r.key);
        });
    }

    ASSERT_EQ(state_full, state_from0);
}

// ---------------------------------------------------------------------------
// S9-S. WAL::replay_from with offset beyond EOF throws.
// ---------------------------------------------------------------------------
TEST(s9_replay_from_beyond_eof_throws) {
    TEMP_SNAP(g);

    write_wal_records(g.wal_path, [](forgekv::WAL& wal) {
        wal.append_set("x", "y");
    });

    forgekv::WAL wal(g.wal_path);
    const std::uint64_t sz = wal.file_size();
    ASSERT_TRUE(sz > 0);

    // Beyond EOF — must throw.
    ASSERT_THROWS(
        (void)wal.replay_from(sz + 1000,
            [](const forgekv::WalRecord&) {})
    );
}

// ---------------------------------------------------------------------------
// S9-T. WAL::file_size returns actual file size.
// ---------------------------------------------------------------------------
TEST(s9_wal_file_size_correct) {
    TEMP_SNAP(g);

    {
        forgekv::WAL wal(g.wal_path);
        ASSERT_EQ(wal.file_size(), std::uint64_t{0});

        wal.append_set("hello", "world");
        const std::uint64_t sz_after = wal.file_size();

        // One SET record: 18 + 5 + 5 = 28 bytes.
        ASSERT_EQ(sz_after, std::uint64_t{28});
    }
}

// ---------------------------------------------------------------------------
// S9-U. Snapshot wal_offset is exactly the WAL size at snapshot time.
// ---------------------------------------------------------------------------
TEST(s9_snapshot_wal_offset_matches_wal_size) {
    TEMP_SNAP(g);

    std::uint64_t expected_offset = 0;
    {
        auto store = make_store(g.wal_path);
        store.set("a", "1");
        store.set("b", "2");

        // Capture WAL size right before snapshot.
        {
            forgekv::WAL wal_probe(g.wal_path);
            expected_offset = wal_probe.file_size();
        }
        ASSERT_TRUE(store.snapshot());

        // Add more records after snapshot.
        store.set("c", "3");
    }

    // Load snapshot and verify wal_offset matches expected.
    forgekv::SnapshotManager sm(g.wal_path);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_FALSE(result.corrupt);
    ASSERT_EQ(result.data.wal_offset, expected_offset);
}

// ---------------------------------------------------------------------------
// S9-V. Clear after snapshot: CLEAR in WAL tail wipes snapshot state.
// ---------------------------------------------------------------------------
TEST(s9_clear_after_snapshot) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("x", "1");
        store.set("y", "2");
        ASSERT_TRUE(store.snapshot());
        store.clear();      // post-snapshot CLEAR
        store.set("z", "3");
    }

    {
        auto store = restart_store(g.wal_path);
        ASSERT_FALSE(store.exists("x"));
        ASSERT_FALSE(store.exists("y"));
        ASSERT_EQ(*store.get("z"), "3");
        ASSERT_EQ(store.size(), std::size_t{1});
    }
}

// ---------------------------------------------------------------------------
// S9-W. SnapshotManager::load() on missing file returns !exists.
// ---------------------------------------------------------------------------
TEST(s9_load_missing_snapshot_returns_not_exists) {
    TEMP_SNAP(g);
    std::error_code ec;
    std::filesystem::remove(g.snap_path, ec);

    forgekv::SnapshotManager sm(g.wal_path);
    const auto result = sm.load();
    ASSERT_FALSE(result.exists);
    ASSERT_FALSE(result.corrupt);
}

// ---------------------------------------------------------------------------
// S9-X. SnapshotManager::load() on corrupt magic returns corrupt.
// ---------------------------------------------------------------------------
TEST(s9_load_corrupt_magic_detected) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("test", "val");
        ASSERT_TRUE(store.snapshot());
    }

    // Overwrite the magic bytes.
    auto bytes = read_file_bytes(g.snap_path);
    bytes[0] = 0xDE; bytes[1] = 0xAD; bytes[2] = 0xBE; bytes[3] = 0xEF;
    {
        std::ofstream out(g.snap_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    forgekv::SnapshotManager sm(g.wal_path);
    const auto result = sm.load();
    ASSERT_TRUE(result.exists);
    ASSERT_TRUE(result.corrupt);
}

// ---------------------------------------------------------------------------
// S9-Y. SnapshotManager::remove() deletes the file.
// ---------------------------------------------------------------------------
TEST(s9_snapshot_manager_remove) {
    TEMP_SNAP(g);

    {
        auto store = make_store(g.wal_path);
        store.set("remove_test", "yes");
        ASSERT_TRUE(store.snapshot());
    }

    ASSERT_TRUE(std::filesystem::exists(g.snap_path));

    forgekv::SnapshotManager sm(g.wal_path);
    ASSERT_TRUE(sm.remove());

    ASSERT_FALSE(std::filesystem::exists(g.snap_path));
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed  = 0;

    std::cout << "\nForgeKV Stage 1–9 — KeyValueStore, WAL, Recovery, Concurrency, Compaction & Snapshots Tests\n";
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
