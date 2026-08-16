// =============================================================================
// ForgeKV — Stage 1: KeyValueStore tests
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
// =============================================================================

#include "forgekv/kv_store.h"

#include <functional>
#include <iostream>
#include <optional>
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

// Registers a test function and runs it by name later.
#define TEST(name)                                                            \
    static void test_##name();                                                \
    static TestRegistrar registrar_##name{#name, test_##name};               \
    static void test_##name()

// -----------------------------------------------------------------------------
// Helper: fresh store per test (avoids shared mutable state between tests)
// -----------------------------------------------------------------------------
// Each test constructs its own local KeyValueStore. There is no global store.

// =============================================================================
// Tests
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
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0;
    int failed = 0;

    std::cout << "\nForgeKV Stage 1 — KeyValueStore Tests\n";
    std::cout << std::string(45, '=') << "\n\n";

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

    std::cout << "\n" << std::string(45, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";

    return (failed == 0) ? 0 : 1;
}
