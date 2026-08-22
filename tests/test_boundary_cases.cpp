// =============================================================================
// ForgeKV — Stage 13: Input and Boundary Tests
// =============================================================================
//
// Tests input boundaries: empty keys/values, single characters, long strings,
// binary-safe content, Unicode (treated as byte sequences), null bytes,
// and special JSON characters through the KV engine layer.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// =============================================================================
// Minimal test harness
// =============================================================================

struct TestCase { std::string name; std::function<void()> fn; };
static std::vector<TestCase>& test_registry() {
    static std::vector<TestCase> r; return r;
}
struct TestRegistrar {
    TestRegistrar(const char* name, std::function<void()> fn) {
        test_registry().push_back({name, std::move(fn)});
    }
};
struct AssertionFailure { std::string message; };

#define ASSERT_TRUE(cond) \
    do { if (!(cond)) throw AssertionFailure{"ASSERT_TRUE failed: " #cond " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_FALSE(cond) \
    do { if ((cond)) throw AssertionFailure{"ASSERT_FALSE failed: " #cond " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_EQ(a,b) \
    do { if ((a)!=(b)) throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_HAS_VALUE(opt) \
    do { if (!(opt).has_value()) throw AssertionFailure{"ASSERT_HAS_VALUE failed: " #opt " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_NO_VALUE(opt) \
    do { if ((opt).has_value()) throw AssertionFailure{"ASSERT_NO_VALUE failed: " #opt " (line " + std::to_string(__LINE__) + ")"}; } while(false)

#define TEST(name) \
    static void test_##name(); \
    static TestRegistrar registrar_##name{#name, test_##name}; \
    static void test_##name()

// =============================================================================
// Helpers
// =============================================================================

struct TempFiles {
    std::string wal;
    std::string snap;
    explicit TempFiles(const std::string& name)
        : wal("test_bc_" + name + ".wal")
        , snap("test_bc_" + name + ".wal.snapshot") {}
    ~TempFiles() {
        std::error_code ec;
        std::filesystem::remove(wal, ec);
        std::filesystem::remove(snap, ec);
    }
};

static forgekv::KeyValueStore make_store(const std::string& path) {
    auto storage = std::make_unique<forgekv::InMemoryStorage>();
    auto wal     = std::make_unique<forgekv::WAL>(path);
    return forgekv::KeyValueStore(std::move(storage), std::move(wal));
}

// =============================================================================
// BC1. Empty string key: set/get/exists/del all work correctly.
// =============================================================================
TEST(bc1_empty_key_roundtrip) {
    TempFiles tf("bc1");
    auto store = make_store(tf.wal);
    store.set("", "empty_key_val");
    ASSERT_TRUE(store.exists(""));
    ASSERT_HAS_VALUE(store.get(""));
    ASSERT_EQ(*store.get(""), "empty_key_val");
    ASSERT_EQ(store.size(), std::size_t{1});
    ASSERT_TRUE(store.del(""));
    ASSERT_FALSE(store.exists(""));
    ASSERT_TRUE(store.empty());
}

// =============================================================================
// BC2. Empty string value: stored and retrieved as empty string.
// =============================================================================
TEST(bc2_empty_value_roundtrip) {
    TempFiles tf("bc2");
    auto store = make_store(tf.wal);
    store.set("k", "");
    ASSERT_TRUE(store.exists("k"));
    ASSERT_HAS_VALUE(store.get("k"));
    ASSERT_EQ(*store.get("k"), "");
}

// =============================================================================
// BC3. Single-character key and value.
// =============================================================================
TEST(bc3_single_char_key_value) {
    TempFiles tf("bc3");
    auto store = make_store(tf.wal);
    store.set("x", "y");
    ASSERT_EQ(*store.get("x"), "y");
    ASSERT_EQ(store.size(), std::size_t{1});
}

// =============================================================================
// BC4. Long key (4096 bytes): stores and retrieves correctly.
// =============================================================================
TEST(bc4_long_key) {
    TempFiles tf("bc4");
    auto store = make_store(tf.wal);
    const std::string long_key(4096, 'k');
    store.set(long_key, "long_key_val");
    ASSERT_TRUE(store.exists(long_key));
    ASSERT_EQ(*store.get(long_key), "long_key_val");
}

// =============================================================================
// BC5. Long value (1 MB): stores and retrieves correctly.
// =============================================================================
TEST(bc5_long_value) {
    TempFiles tf("bc5");
    auto store = make_store(tf.wal);
    const std::string long_val(1024 * 1024, 'V');
    store.set("bigkey", long_val);
    ASSERT_HAS_VALUE(store.get("bigkey"));
    ASSERT_EQ(*store.get("bigkey"), long_val);
}

// =============================================================================
// BC6. Key containing null bytes: binary-safe storage.
// =============================================================================
TEST(bc6_null_bytes_in_key) {
    TempFiles tf("bc6");
    auto store = make_store(tf.wal);
    const std::string null_key("key\0with\0nulls", 14);
    store.set(null_key, "null_key_value");
    ASSERT_TRUE(store.exists(null_key));
    ASSERT_EQ(*store.get(null_key), "null_key_value");
    ASSERT_EQ(null_key.size(), std::size_t{14});
}

// =============================================================================
// BC7. Value containing null bytes: binary-safe storage.
// =============================================================================
TEST(bc7_null_bytes_in_value) {
    TempFiles tf("bc7");
    auto store = make_store(tf.wal);
    const std::string null_val("val\0with\0nulls", 14);
    store.set("nv_key", null_val);
    ASSERT_HAS_VALUE(store.get("nv_key"));
    ASSERT_EQ(*store.get("nv_key"), null_val);
}

// =============================================================================
// BC8. Key containing special JSON characters (", \, newline, etc.).
// =============================================================================
TEST(bc8_json_special_chars_in_key) {
    TempFiles tf("bc8");
    auto store = make_store(tf.wal);
    const std::string special_key = "key\"with\\special\nchars\r\t";
    store.set(special_key, "value");
    ASSERT_TRUE(store.exists(special_key));
    ASSERT_EQ(*store.get(special_key), "value");
}

// =============================================================================
// BC9. Value containing all control characters 0x00-0x1F.
// =============================================================================
TEST(bc9_control_chars_in_value) {
    TempFiles tf("bc9");
    auto store = make_store(tf.wal);
    std::string ctrl_val;
    for (int c = 0; c <= 0x1F; ++c) {
        ctrl_val += static_cast<char>(c);
    }
    store.set("ctrl", ctrl_val);
    ASSERT_HAS_VALUE(store.get("ctrl"));
    ASSERT_EQ(*store.get("ctrl"), ctrl_val);
    ASSERT_EQ(store.get("ctrl")->size(), std::size_t{32});
}

// =============================================================================
// BC10. Unicode / multibyte UTF-8 key and value (treated as byte sequences).
// =============================================================================
TEST(bc10_utf8_key_value) {
    TempFiles tf("bc10");
    auto store = make_store(tf.wal);
    // UTF-8 encoded strings.
    const std::string utf8_key   = "\xE2\x83\x9E\xF0\x9F\x94\xA5"; // ⃞🔥
    const std::string utf8_value = "\xC3\xA9\xC3\xA0\xC3\xBC";      // éàü
    store.set(utf8_key, utf8_value);
    ASSERT_TRUE(store.exists(utf8_key));
    ASSERT_EQ(*store.get(utf8_key), utf8_value);
}

// =============================================================================
// BC11. Key with only whitespace characters.
// =============================================================================
TEST(bc11_whitespace_key) {
    TempFiles tf("bc11");
    auto store = make_store(tf.wal);
    const std::string ws_key = "   \t\n\r   ";
    store.set(ws_key, "whitespace_key");
    ASSERT_TRUE(store.exists(ws_key));
    ASSERT_EQ(*store.get(ws_key), "whitespace_key");
}

// =============================================================================
// BC12. Very long key and value survive WAL round-trip.
// =============================================================================
TEST(bc12_long_key_value_wal_roundtrip) {
    TempFiles tf("bc12");
    const std::string long_key(2048, 'K');
    const std::string long_val(65536, 'V');
    {
        auto store = make_store(tf.wal);
        store.set(long_key, long_val);
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_HAS_VALUE(store.get(long_key));
        ASSERT_EQ(*store.get(long_key), long_val);
    }
}

// =============================================================================
// BC13. Empty key + empty value survive WAL round-trip.
// =============================================================================
TEST(bc13_empty_key_value_wal_roundtrip) {
    TempFiles tf("bc13");
    {
        auto store = make_store(tf.wal);
        store.set("", "");
    }
    {
        auto store = make_store(tf.wal);
        ASSERT_TRUE(store.exists(""));
        ASSERT_EQ(*store.get(""), "");
    }
}

// =============================================================================
// BC14. Two keys that differ only in a single bit are distinct.
// =============================================================================
TEST(bc14_near_identical_keys_distinct) {
    TempFiles tf("bc14");
    auto store = make_store(tf.wal);
    const std::string key_a = "abc";
    const std::string key_b = "abC"; // differs at last char (case)
    store.set(key_a, "valA");
    store.set(key_b, "valB");
    ASSERT_EQ(store.size(), std::size_t{2});
    ASSERT_EQ(*store.get(key_a), "valA");
    ASSERT_EQ(*store.get(key_b), "valB");
}

// =============================================================================
// BC15. set_with_ttl with empty value is valid (if ttl > 0).
// =============================================================================
TEST(bc15_ttl_empty_value) {
    TempFiles tf("bc15");
    auto store = make_store(tf.wal);
    store.set_with_ttl("ttl_empty", "", 3600.0);
    ASSERT_TRUE(store.exists("ttl_empty"));
    ASSERT_EQ(*store.get("ttl_empty"), "");
    ASSERT_TRUE(store.ttl("ttl_empty") > 0.0);
}

// =============================================================================
// BC16. Key with pipe and pipe-like delimiters (were problematic in text WAL).
// =============================================================================
TEST(bc16_pipe_delimiters_in_key_value) {
    TempFiles tf("bc16");
    auto store = make_store(tf.wal);
    const std::string pipe_key   = "k|e|y|pipe";
    const std::string pipe_value = "v|a|l|pipe";
    store.set(pipe_key, pipe_value);
    ASSERT_EQ(*store.get(pipe_key), pipe_value);

    // Survives restart.
    {
        auto s2 = make_store(tf.wal);
        ASSERT_EQ(*s2.get(pipe_key), pipe_value);
    }
}

// =============================================================================
// BC17. Very large number of distinct keys (1000): all accessible.
// =============================================================================
TEST(bc17_large_key_count) {
    TempFiles tf("bc17");
    auto store = make_store(tf.wal);
    const int N = 1000;
    for (int i = 0; i < N; ++i) {
        store.set("boundary_key_" + std::to_string(i),
                  "boundary_val_" + std::to_string(i));
    }
    ASSERT_EQ(store.size(), static_cast<std::size_t>(N));
    for (int i = 0; i < N; ++i) {
        auto v = store.get("boundary_key_" + std::to_string(i));
        ASSERT_HAS_VALUE(v);
        ASSERT_EQ(*v, "boundary_val_" + std::to_string(i));
    }
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Boundary Cases Tests\n";
    std::cout << std::string(42, '=') << "\n\n";
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
    std::cout << "\n" << std::string(42, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
