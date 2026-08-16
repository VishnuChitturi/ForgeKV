// =============================================================================
// ForgeKV — Stage 1 + Stage 2 + Stage 3: KeyValueStore tests
// =============================================================================
//
// Minimal self-contained test harness — no external framework required.
//
// Each TEST() block is a function registered at startup via a global list.
// The runner executes every registered test, catches assertion failures, and
// reports a summary. Exit code 0 = all tests passed, 1 = at least one failed.
//
// ASSERT_TRUE(cond)        — fail if cond is false
// ASSERT_EQ(a, b)          — fail if a != b
// ASSERT_FALSE(cond)       — fail if cond is true
// ASSERT_HAS_VALUE(opt)    — fail if optional is empty
// ASSERT_NO_VALUE(opt)     — fail if optional has a value
// ASSERT_THROWS(expr)      — fail if expr does NOT throw
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/storage.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

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

#define ASSERT_TRUE(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            throw AssertionFailure{"ASSERT_TRUE failed: " #cond              \
                                   " (line " + std::to_string(__LINE__) + ")"};  \
        }                                                                     \
    } while (false)

#define ASSERT_FALSE(cond)                                                   \
    do {                                                                      \
        if ((cond)) {                                                         \
            throw AssertionFailure{"ASSERT_FALSE failed: " #cond             \
                                   " (line " + std::to_string(__LINE__) + ")"};  \
        }                                                                     \
    } while (false)

#define ASSERT_EQ(a, b)                                                      \
    do {                                                                      \
        if ((a) != (b)) {                                                     \
            throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b        \
                                   " (line " + std::to_string(__LINE__) + ")"};  \
        }                                                                     \
    } while (false)

#define ASSERT_HAS_VALUE(opt)                                                \
    do {                                                                      \
        if (!(opt).has_value()) {                                             \
            throw AssertionFailure{"ASSERT_HAS_VALUE failed: " #opt          \
                                   " is empty (line " + std::to_string(__LINE__) + ")"}; \
        }                                                                     \
    } while (false)

#define ASSERT_NO_VALUE(opt)                                                 \
    do {                                                                      \
        if ((opt).has_value()) {                                              \
            throw AssertionFailure{"ASSERT_NO_VALUE failed: " #opt           \
                                   " has a value (line " + std::to_string(__LINE__) + ")"}; \
        }                                                                     \
    } while (false)

// ASSERT_THROWS: verify that an expression throws any std::exception.
// Fails if the expression completes without throwing.
#define ASSERT_THROWS(expr)                                                  \
    do {                                                                      \
        bool threw = false;                                                   \
        try { (expr); }                                                       \
        catch (const std::exception&) { threw = true; }                      \
        catch (...) { threw = true; }                                         \
        if (!threw) {                                                         \
            throw AssertionFailure{                                           \
                "ASSERT_THROWS failed: expression did not throw: " #expr     \
                " (line " + std::to_string(__LINE__) + ")"};                 \
        }                                                                     \
    } while (false)

// Registers a test function and runs it by name later.
#define TEST(name)                                                            \
    static void test_##name();                                                \
    static TestRegistrar registrar_##name{#name, test_##name};               \
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
    store.set("age",  "21");
    store.set("city", "Bengaluru");

    auto name = store.get("name");
    auto age  = store.get("age");
    auto city = store.get("city");

    ASSERT_HAS_VALUE(name); ASSERT_EQ(*name, "Vishnu");
    ASSERT_HAS_VALUE(age);  ASSERT_EQ(*age,  "21");
    ASSERT_HAS_VALUE(city); ASSERT_EQ(*city, "Bengaluru");

    ASSERT_EQ(store.size(), std::size_t{3});
}

// 3. Updating an existing key — value is overwritten, key count stays the same
TEST(update_existing_key) {
    forgekv::KeyValueStore store;
    store.set("name", "Alice");
    ASSERT_EQ(*store.get("name"), "Alice");

    store.set("name", "Bob");
    ASSERT_EQ(*store.get("name"), "Bob");

    // Only one key should exist — SET is upsert, not insert
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
    ASSERT_TRUE(removed);                         // del should return true
    ASSERT_NO_VALUE(store.get("key"));            // key must be gone
    ASSERT_EQ(store.size(), std::size_t{0});
}

// 6. DELETE of a missing key — must be a safe no-op, not crash
TEST(delete_missing_key) {
    forgekv::KeyValueStore store;
    bool removed = store.del("ghost");
    ASSERT_FALSE(removed);    // returns false — key was not present
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
    // After 100 sets on the same key, only the last value survives
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
    ASSERT_EQ(store.size(), std::size_t{1});   // size must not change
}

// 14. del() is idempotent — calling twice on same key is safe
TEST(double_delete) {
    forgekv::KeyValueStore store;
    store.set("temp", "data");
    ASSERT_TRUE(store.del("temp"));   // first delete: key existed
    ASSERT_FALSE(store.del("temp"));  // second delete: key already gone
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
//
// These tests verify:
//   1. InMemoryStorage works correctly in isolation.
//   2. InMemoryStorage is correctly accessible through the Storage interface.
//   3. KeyValueStore still satisfies all Stage 1 contracts through the
//      storage abstraction layer.
//   4. KeyValueStore accepts a custom Storage implementation via injection,
//      demonstrating the abstraction is real (using a minimal fake store).
// =============================================================================

// -----------------------------------------------------------------------------
// Minimal fake Storage implementation
// -----------------------------------------------------------------------------
// FakeStorage is a stand-alone Storage that always reports empty and no-ops
// on all writes. Its only purpose is to prove that KeyValueStore accepts
// *any* Storage, not just InMemoryStorage. No external mocking library needed.
// -----------------------------------------------------------------------------

class FakeStorage final : public forgekv::Storage {
public:
    // Counts how many times each method was called — lets tests verify
    // that KeyValueStore is actually forwarding calls to storage_.
    mutable int set_calls    = 0;
    mutable int get_calls    = 0;
    mutable int del_calls    = 0;
    mutable int exists_calls = 0;
    mutable int size_calls   = 0;
    mutable int empty_calls  = 0;
    mutable int clear_calls  = 0;

    void set(const std::string&, const std::string&) override { ++set_calls; }

    std::optional<std::string> get(const std::string&) const override {
        ++get_calls;
        return std::nullopt;
    }

    bool del(const std::string&) override { ++del_calls; return false; }

    bool exists(const std::string&) const override {
        ++exists_calls;
        return false;
    }

    std::size_t size() const override { ++size_calls; return 0; }

    bool empty() const override { ++empty_calls; return true; }

    void clear() override { ++clear_calls; }
};

// =============================================================================
// InMemoryStorage — standalone tests (not through KeyValueStore)
// =============================================================================

// S2-1. InMemoryStorage basic set/get round-trip
TEST(storage_set_then_get) {
    forgekv::InMemoryStorage s;
    s.set("key", "value");
    auto result = s.get("key");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "value");
}

// S2-2. InMemoryStorage — update (overwrite) existing key
TEST(storage_update_key) {
    forgekv::InMemoryStorage s;
    s.set("lang", "C");
    s.set("lang", "C++");
    auto result = s.get("lang");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "C++");
    ASSERT_EQ(s.size(), std::size_t{1});
}

// S2-3. InMemoryStorage — get missing key returns nullopt
TEST(storage_get_missing) {
    forgekv::InMemoryStorage s;
    ASSERT_NO_VALUE(s.get("absent"));
}

// S2-4. InMemoryStorage — del existing key returns true, key gone
TEST(storage_del_existing) {
    forgekv::InMemoryStorage s;
    s.set("x", "1");
    ASSERT_TRUE(s.del("x"));
    ASSERT_NO_VALUE(s.get("x"));
    ASSERT_EQ(s.size(), std::size_t{0});
}

// S2-5. InMemoryStorage — del missing key returns false, no crash
TEST(storage_del_missing) {
    forgekv::InMemoryStorage s;
    ASSERT_FALSE(s.del("ghost"));
}

// S2-6. InMemoryStorage — exists
TEST(storage_exists) {
    forgekv::InMemoryStorage s;
    ASSERT_FALSE(s.exists("k"));
    s.set("k", "v");
    ASSERT_TRUE(s.exists("k"));
    s.del("k");
    ASSERT_FALSE(s.exists("k"));
}

// S2-7. InMemoryStorage — size and empty
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

// S2-8. InMemoryStorage — clear removes all entries
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

// S2-9. set/get through Storage pointer
TEST(storage_interface_set_get) {
    std::unique_ptr<forgekv::Storage> s =
        std::make_unique<forgekv::InMemoryStorage>();
    s->set("hello", "world");
    auto result = s->get("hello");
    ASSERT_HAS_VALUE(result);
    ASSERT_EQ(*result, "world");
}

// S2-10. del/exists through Storage pointer
TEST(storage_interface_del_exists) {
    std::unique_ptr<forgekv::Storage> s =
        std::make_unique<forgekv::InMemoryStorage>();
    s->set("tmp", "data");
    ASSERT_TRUE(s->exists("tmp"));
    ASSERT_TRUE(s->del("tmp"));
    ASSERT_FALSE(s->exists("tmp"));
}

// S2-11. size/empty/clear through Storage pointer
TEST(storage_interface_size_empty_clear) {
    std::unique_ptr<forgekv::Storage> s =
        std::make_unique<forgekv::InMemoryStorage>();
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

// S2-12. KeyValueStore default ctor — still works the same as Stage 1
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

// S2-13. KeyValueStore — updating existing key still works through abstraction
TEST(kv_update_through_abstraction) {
    forgekv::KeyValueStore store;
    store.set("k", "old");
    store.set("k", "new");
    ASSERT_EQ(*store.get("k"), "new");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// S2-14. KeyValueStore — clear through abstraction
TEST(kv_clear_through_abstraction) {
    forgekv::KeyValueStore store;
    store.set("x", "1");
    store.set("y", "2");
    store.clear();
    ASSERT_TRUE(store.empty());
    ASSERT_NO_VALUE(store.get("x"));
}

// S2-15. KeyValueStore — missing key returns nullopt through abstraction
TEST(kv_missing_key_through_abstraction) {
    forgekv::KeyValueStore store;
    ASSERT_NO_VALUE(store.get("absent"));
    ASSERT_FALSE(store.exists("absent"));
}

// S2-16. KeyValueStore — del missing key is safe through abstraction
TEST(kv_del_missing_through_abstraction) {
    forgekv::KeyValueStore store;
    ASSERT_FALSE(store.del("no-such-key"));
    ASSERT_EQ(store.size(), std::size_t{0});
}

// =============================================================================
// Dependency injection — FakeStorage
// =============================================================================

// S2-17. KeyValueStore forwards set() to the injected storage
TEST(di_forwards_set) {
    auto* fake = new FakeStorage();
    auto ptr   = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    store.set("k", "v");
    ASSERT_EQ(fake->set_calls, 1);
}

// S2-18. KeyValueStore forwards get() to the injected storage
TEST(di_forwards_get) {
    auto* fake = new FakeStorage();
    auto ptr   = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    auto result = store.get("k");
    ASSERT_NO_VALUE(result);      // FakeStorage always returns nullopt
    ASSERT_EQ(fake->get_calls, 1);
}

// S2-19. KeyValueStore del() on a missing key — does NOT forward to storage.
//
// Stage 3 design: del() checks storage_->exists() first.
// If the key is absent, del() returns false immediately without calling
// storage_->del(). This is because del() only writes a WAL record (and
// only removes from storage) when the key actually exists — a delete of a
// non-existent key is a no-op that does not change state.
//
// Consequence for FakeStorage (which always returns false from exists()):
// del_calls remains 0, and exists_calls is 1.
TEST(di_forwards_del) {
    auto* fake = new FakeStorage();
    auto ptr   = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    bool removed = store.del("k");
    ASSERT_FALSE(removed);             // key not present — del is a no-op
    ASSERT_EQ(fake->exists_calls, 1);  // exists() was checked
    ASSERT_EQ(fake->del_calls, 0);     // del() not forwarded when key absent
}

// S2-20. KeyValueStore forwards exists() to the injected storage
TEST(di_forwards_exists) {
    auto* fake = new FakeStorage();
    auto ptr   = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));
    bool found = store.exists("k");
    ASSERT_FALSE(found);          // FakeStorage always returns false
    ASSERT_EQ(fake->exists_calls, 1);
}

// S2-21. KeyValueStore forwards size()/empty()/clear() to the injected storage
TEST(di_forwards_utility_methods) {
    auto* fake = new FakeStorage();
    auto ptr   = std::unique_ptr<forgekv::Storage>(fake);
    forgekv::KeyValueStore store(std::move(ptr));

    ASSERT_EQ(store.size(), std::size_t{0});
    ASSERT_TRUE(store.empty());
    store.clear();

    ASSERT_EQ(fake->size_calls,  1);
    ASSERT_EQ(fake->empty_calls, 1);
    ASSERT_EQ(fake->clear_calls, 1);
}

// =============================================================================
// Stage 3 tests — Write-Ahead Log (WAL)
// =============================================================================
//
// These tests verify:
//   1. WAL creates a file if it does not exist.
//   2. SET appends the expected textual record (SET|key|value).
//   3. Multiple SET operations append in order.
//   4. DEL appends the expected record (DEL|key).
//   5. CLEAR appends the CLEAR record.
//   6. Existing WAL contents are preserved when reopening.
//   7. KeyValueStore::set() writes to WAL before updating Storage.
//   8. KeyValueStore::del() writes the correct WAL record.
//   9. Read-only operations do NOT append WAL entries.
//  10. WAL write failures propagate; in-memory state is NOT mutated.
//  11. Full DI constructor: storage + WAL injected together.
//  12. del() on a non-existent key does NOT write a WAL record.
//
// All tests use temporary file paths and clean up on exit.
// The std::filesystem library (C++17/20) is used for reliable cleanup.
// =============================================================================

// Helper: read the entire content of a file into a string.
static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("read_file: cannot open: " + path);
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Helper: RAII guard that deletes a file on scope exit.
// Ensures test files are always cleaned up even when a test throws.
struct TempFile {
    std::string path;
    explicit TempFile(std::string p) : path(std::move(p)) {}
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        // Ignore errors — file may not exist if test failed early.
    }
};

// Unique temp WAL path per test — avoids collisions when tests run in the
// same process. Uses __LINE__ to guarantee uniqueness across tests.
#define TEMP_WAL(name) \
    TempFile name{"test_wal_" #name ".wal"}

// ---------------------------------------------------------------------------
// S3-1. WAL creates a new file if it does not exist.
// ---------------------------------------------------------------------------
TEST(s3_wal_creates_file) {
    TEMP_WAL(guard);
    // Ensure the file does not already exist
    std::filesystem::remove(guard.path);

    {
        forgekv::WAL wal(guard.path);
        // Simply opening the WAL should create the file
    }

    ASSERT_TRUE(std::filesystem::exists(guard.path));
}

// ---------------------------------------------------------------------------
// S3-2. SET appends a correctly formatted record: "SET|key|value\n"
// ---------------------------------------------------------------------------
TEST(s3_set_record_format) {
    TEMP_WAL(guard);

    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
    }

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "SET|name|Vishnu\n");
}

// ---------------------------------------------------------------------------
// S3-3. Multiple SET operations append records in order.
// ---------------------------------------------------------------------------
TEST(s3_multiple_sets_in_order) {
    TEMP_WAL(guard);

    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
        wal.append_set("age",  "21");
        wal.append_set("city", "Bengaluru");
    }

    const std::string content = read_file(guard.path);
    const std::string expected =
        "SET|name|Vishnu\n"
        "SET|age|21\n"
        "SET|city|Bengaluru\n";
    ASSERT_EQ(content, expected);
}

// ---------------------------------------------------------------------------
// S3-4. DEL appends a correctly formatted record: "DEL|key\n"
// ---------------------------------------------------------------------------
TEST(s3_del_record_format) {
    TEMP_WAL(guard);

    {
        forgekv::WAL wal(guard.path);
        wal.append_del("age");
    }

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "DEL|age\n");
}

// ---------------------------------------------------------------------------
// S3-5. CLEAR appends exactly "CLEAR\n"
// ---------------------------------------------------------------------------
TEST(s3_clear_record_format) {
    TEMP_WAL(guard);

    {
        forgekv::WAL wal(guard.path);
        wal.append_clear();
    }

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "CLEAR\n");
}

// ---------------------------------------------------------------------------
// S3-6. Reopening WAL appends to existing content (does not truncate).
// ---------------------------------------------------------------------------
TEST(s3_reopen_preserves_content) {
    TEMP_WAL(guard);

    // First session: write a SET
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("key1", "val1");
    }

    // Second session: open the same file and write another record
    {
        forgekv::WAL wal(guard.path);
        wal.append_set("key2", "val2");
    }

    const std::string content = read_file(guard.path);
    const std::string expected =
        "SET|key1|val1\n"
        "SET|key2|val2\n";
    ASSERT_EQ(content, expected);
}

// ---------------------------------------------------------------------------
// S3-7. A mixed sequence produces the correct WAL.
// ---------------------------------------------------------------------------
TEST(s3_mixed_sequence) {
    TEMP_WAL(guard);

    {
        forgekv::WAL wal(guard.path);
        wal.append_set("name", "Vishnu");
        wal.append_set("age",  "21");
        wal.append_del("age");
        wal.append_clear();
    }

    const std::string content = read_file(guard.path);
    const std::string expected =
        "SET|name|Vishnu\n"
        "SET|age|21\n"
        "DEL|age\n"
        "CLEAR\n";
    ASSERT_EQ(content, expected);
}

// ---------------------------------------------------------------------------
// S3-8. WAL opening failure throws std::runtime_error.
//        Use a path that cannot be created (directory as filename).
// ---------------------------------------------------------------------------
TEST(s3_wal_open_failure_throws) {
    // A path containing a null byte is invalid on POSIX and Windows.
    // An alternative: use a nonexistent deeply nested path.
    // The most portable: try to open a directory as a file.
    // We create a temp directory and then try to open it as a WAL file.
    const std::string bad_path = "/tmp/forgekv_no_such_dir_xyz/wal.log";
    ASSERT_THROWS(forgekv::WAL{bad_path});
}

// ---------------------------------------------------------------------------
// S3-9. KeyValueStore::set() writes the WAL record and updates storage.
// ---------------------------------------------------------------------------
TEST(s3_kvstore_set_writes_wal_and_updates_storage) {
    TEMP_WAL(guard);

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw    = storage.get();   // retain raw ptr to inspect later
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);

    forgekv::KeyValueStore store(std::move(storage), std::move(wal));
    store.set("name", "Vishnu");

    // WAL should have the record
    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "SET|name|Vishnu\n");

    // In-memory storage should have the value
    auto val = raw->get("name");
    ASSERT_HAS_VALUE(val);
    ASSERT_EQ(*val, "Vishnu");
}

// ---------------------------------------------------------------------------
// S3-10. KeyValueStore::del() writes the WAL record and removes from storage.
// ---------------------------------------------------------------------------
TEST(s3_kvstore_del_writes_wal_and_updates_storage) {
    TEMP_WAL(guard);

    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto* raw    = storage.get();
    auto wal     = std::make_unique<forgekv::WAL>(guard.path);

    forgekv::KeyValueStore store(std::move(storage), std::move(wal));
    store.set("city", "Bengaluru");   // also writes SET|city|Bengaluru to WAL

    bool removed = store.del("city");
    ASSERT_TRUE(removed);

    // WAL should contain both the SET and the DEL
    const std::string content = read_file(guard.path);
    ASSERT_EQ(content,
              "SET|city|Bengaluru\n"
              "DEL|city\n");

    // In-memory storage should no longer have the key
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

    bool removed = store.del("ghost");  // key never existed
    ASSERT_FALSE(removed);

    // WAL file should be empty — no record for a no-op del
    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "");
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
    store.clear();

    ASSERT_TRUE(store.empty());

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content,
              "SET|a|1\n"
              "SET|b|2\n"
              "CLEAR\n");
}

// ---------------------------------------------------------------------------
// S3-13. Read-only operations (get, exists, size, empty) do NOT write WAL.
// ---------------------------------------------------------------------------
TEST(s3_readonly_ops_no_wal_write) {
    TEMP_WAL(guard);

    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    // Perform only read operations (cast to void to silence nodiscard warnings)
    (void)store.get("absent");
    (void)store.exists("absent");
    (void)store.size();
    (void)store.empty();

    // WAL file must be empty
    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "");
}

// ---------------------------------------------------------------------------
// S3-14. Full DI: Storage and WAL are both injected.
//        Verify the store works end-to-end with injected dependencies.
// ---------------------------------------------------------------------------
TEST(s3_full_di_storage_and_wal) {
    TEMP_WAL(guard);

    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    store.set("project", "ForgeKV");
    store.set("stage",   "3");
    store.del("stage");

    ASSERT_EQ(*store.get("project"), "ForgeKV");
    ASSERT_FALSE(store.exists("stage"));

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content,
              "SET|project|ForgeKV\n"
              "SET|stage|3\n"
              "DEL|stage\n");
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
// S3-16. Empty value in SET is written correctly: "SET|key|\n"
// ---------------------------------------------------------------------------
TEST(s3_set_empty_value) {
    TEMP_WAL(guard);

    forgekv::WAL wal(guard.path);
    wal.append_set("emptyval", "");

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "SET|emptyval|\n");
}

// ---------------------------------------------------------------------------
// S3-17. Empty key in SET is written correctly: "SET||value\n"
// ---------------------------------------------------------------------------
TEST(s3_set_empty_key) {
    TEMP_WAL(guard);

    forgekv::WAL wal(guard.path);
    wal.append_set("", "somevalue");

    const std::string content = read_file(guard.path);
    ASSERT_EQ(content, "SET||somevalue\n");
}

// ---------------------------------------------------------------------------
// S3-18. Stage 1 API still works when WAL is injected (end-to-end).
//        Verifies backward compatibility across all seven Stage 1 operations.
// ---------------------------------------------------------------------------
TEST(s3_stage1_api_works_with_wal) {
    TEMP_WAL(guard);

    auto wal = std::make_unique<forgekv::WAL>(guard.path);
    forgekv::KeyValueStore store(
        std::make_unique<forgekv::InMemoryStorage>(),
        std::move(wal)
    );

    // set / get
    store.set("lang", "C++");
    ASSERT_EQ(*store.get("lang"), "C++");

    // exists
    ASSERT_TRUE(store.exists("lang"));

    // size / empty
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_FALSE(store.empty());

    // del (existing)
    ASSERT_TRUE(store.del("lang"));
    ASSERT_FALSE(store.exists("lang"));
    ASSERT_TRUE(store.empty());

    // del (missing)
    ASSERT_FALSE(store.del("lang"));

    // set + clear
    store.set("x", "1");
    store.clear();
    ASSERT_TRUE(store.empty());
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed = 0;

    std::cout << "\nForgeKV Stage 1 + 2 + 3 — KeyValueStore & WAL Tests\n";
    std::cout << std::string(55, '=') << "\n\n";

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

    std::cout << "\n" << std::string(55, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";

    return (failed == 0) ? 0 : 1;
}
