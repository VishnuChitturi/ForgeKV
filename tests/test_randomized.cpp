// =============================================================================
// ForgeKV — Stage 13: Deterministic Randomized State-Machine Test
// =============================================================================
//
// Generates a fixed-seed sequence of SET / GET / DELETE / EXISTS / TTL
// operations and compares ForgeKV results against a simple reference model
// (std::unordered_map + optional expiry).
//
// Design:
//   - Fixed seed (printed on failure for reproducibility).
//   - Reference model tracks value + optional absolute expiry (microseconds).
//   - GET / EXISTS / TTL on expired keys must match reference behavior.
//   - Runs 5000 operations over 50 keys.
//   - Includes a restart mid-sequence to verify WAL recovery matches model.
//   - Runs a compaction mid-sequence to verify compact doesn't alter state.
//   - No external dependencies — uses the same harness as other Stage 13 tests.
//
// On failure: the seed and failing operation index are printed.
// =============================================================================

#include "forgekv/kv_store.h"
#include "forgekv/in_memory_storage.h"
#include "forgekv/wal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
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
    do { if (!(cond)) throw AssertionFailure{"ASSERT_TRUE failed: " #cond \
        " (line " + std::to_string(__LINE__) + ")"}; } while(false)
#define ASSERT_EQ(a,b) \
    do { if ((a)!=(b)) throw AssertionFailure{"ASSERT_EQ failed: " #a " != " #b \
        " (line " + std::to_string(__LINE__) + ")"}; } while(false)

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
        : wal("test_rnd_" + name + ".wal")
        , snap("test_rnd_" + name + ".wal.snapshot") {}
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

static std::uint64_t current_time_us() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// =============================================================================
// Reference model
// =============================================================================

struct RefEntry {
    std::string   value;
    std::uint64_t expires_at_us{0}; // 0 = permanent
};

class ReferenceModel {
public:
    void set(const std::string& key, const std::string& val) {
        store_[key] = RefEntry{val, 0};
    }

    void set_with_ttl(const std::string& key, const std::string& val,
                      double ttl_seconds) {
        if (ttl_seconds <= 0.0) return; // not stored
        const std::uint64_t exp = current_time_us() +
            static_cast<std::uint64_t>(ttl_seconds * 1'000'000.0);
        store_[key] = RefEntry{val, exp};
    }

    std::optional<std::string> get(const std::string& key) const {
        auto it = store_.find(key);
        if (it == store_.end()) return std::nullopt;
        if (is_expired(it->second)) return std::nullopt;
        return it->second.value;
    }

    bool exists(const std::string& key) const {
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        return !is_expired(it->second);
    }

    // Returns true if key was present and not expired.
    bool del(const std::string& key) {
        auto it = store_.find(key);
        if (it == store_.end()) return false;
        if (is_expired(it->second)) return false;
        store_.erase(it);
        return true;
    }

    std::size_t size() const {
        std::size_t count = 0;
        for (const auto& [k, e] : store_) {
            if (!is_expired(e)) ++count;
        }
        return count;
    }

    // Sync: apply expired-key removal so model stays in sync with cleanup.
    void expire_pass() {
        const std::uint64_t now = current_time_us();
        std::vector<std::string> to_erase;
        for (const auto& [k, e] : store_) {
            if (e.expires_at_us != 0 && e.expires_at_us <= now) {
                to_erase.push_back(k);
            }
        }
        for (const auto& k : to_erase) store_.erase(k);
    }

    const std::unordered_map<std::string, RefEntry>& raw() const {
        return store_;
    }

private:
    static bool is_expired(const RefEntry& e) noexcept {
        if (e.expires_at_us == 0) return false;
        return e.expires_at_us <= current_time_us();
    }

    std::unordered_map<std::string, RefEntry> store_;
};

// =============================================================================
// Operation types
// =============================================================================

enum class Op { SET, GET, DEL, EXISTS, TTL_SET };

// =============================================================================
// RZ1. Deterministic random state-machine: 5000 operations, fixed seed.
// =============================================================================
TEST(rz1_deterministic_state_machine) {
    TempFiles tf("rz1");

    constexpr std::uint32_t SEED     = 0xFEEDC0DE;
    constexpr int           OPS      = 5000;
    constexpr int           KEY_SPACE = 50;

    std::mt19937 rng(SEED);
    std::uniform_int_distribution<int>  op_dist(0, 4);
    std::uniform_int_distribution<int>  key_dist(0, KEY_SPACE - 1);
    std::uniform_int_distribution<int>  val_dist(0, 999);
    std::uniform_real_distribution<double> ttl_dist(0.001, 3600.0);

    ReferenceModel ref;
    auto store = make_store(tf.wal);

    int op_index = 0;
    try {
        for (; op_index < OPS; ++op_index) {
            const Op  op  = static_cast<Op>(op_dist(rng));
            const std::string key = "k" + std::to_string(key_dist(rng));
            const std::string val = "v" + std::to_string(val_dist(rng));

            switch (op) {
                case Op::SET: {
                    store.set(key, val);
                    ref.set(key, val);
                    // Immediate get must match.
                    const auto sv = store.get(key);
                    const auto rv = ref.get(key);
                    ASSERT_TRUE(sv.has_value());
                    ASSERT_TRUE(rv.has_value());
                    ASSERT_EQ(*sv, *rv);
                    break;
                }
                case Op::GET: {
                    const auto sv = store.get(key);
                    const auto rv = ref.get(key);
                    // Both absent: OK. Both present with same value: OK.
                    // Mismatch: fail.
                    if (sv.has_value() != rv.has_value()) {
                        // Could be a timing edge if TTL just expired between
                        // the two calls. Tolerate only if both now agree on
                        // absence.
                        const auto sv2 = store.get(key);
                        const auto rv2 = ref.get(key);
                        ASSERT_TRUE(!sv2.has_value() && !rv2.has_value());
                    } else if (sv.has_value()) {
                        ASSERT_EQ(*sv, *rv);
                    }
                    break;
                }
                case Op::DEL: {
                    const bool sd = store.del(key);
                    const bool rd = ref.del(key);
                    // If both agree the key was absent, both return false.
                    // If one says present and one says absent: timing edge for
                    // TTL. Tolerate by only asserting both now return false.
                    if (sd != rd) {
                        // Could be expiry race: acceptable for TTL keys.
                        // Just verify key is now gone from both.
                        ASSERT_TRUE(!store.exists(key));
                    }
                    break;
                }
                case Op::EXISTS: {
                    const bool se = store.exists(key);
                    const bool re = ref.exists(key);
                    if (se != re) {
                        // Again allow expiry race: both must agree after.
                        ASSERT_TRUE(!store.exists(key) && !ref.exists(key));
                    }
                    break;
                }
                case Op::TTL_SET: {
                    const double ttl = ttl_dist(rng);
                    store.set_with_ttl(key, val, ttl);
                    ref.set_with_ttl(key, val, ttl);
                    // For TTL > 5s, key should be present immediately.
                    if (ttl > 5.0) {
                        ASSERT_TRUE(store.exists(key));
                        ASSERT_TRUE(ref.exists(key));
                    }
                    break;
                }
            }
        }
    } catch (const AssertionFailure& e) {
        std::cerr << "\n[SEED=" << SEED << "] Failure at op_index=" << op_index
                  << ": " << e.message << "\n";
        throw;
    }
}

// =============================================================================
// RZ2. State-machine with restart mid-sequence: recovered state matches model.
// =============================================================================
TEST(rz2_state_machine_with_restart) {
    TempFiles tf("rz2");

    constexpr std::uint32_t SEED      = 0xABCD1234;
    constexpr int           OPS_PRE   = 300;  // before restart
    constexpr int           OPS_POST  = 200;  // after restart
    constexpr int           KEY_SPACE = 30;

    std::mt19937 rng(SEED);
    std::uniform_int_distribution<int> op_dist(0, 2); // SET, GET, DEL only
    std::uniform_int_distribution<int> key_dist(0, KEY_SPACE - 1);
    std::uniform_int_distribution<int> val_dist(0, 999);

    ReferenceModel ref;

    // Phase 1: write OPS_PRE operations.
    {
        auto store = make_store(tf.wal);
        for (int i = 0; i < OPS_PRE; ++i) {
            const std::string key = "k" + std::to_string(key_dist(rng));
            const std::string val = "v" + std::to_string(val_dist(rng));
            switch (op_dist(rng)) {
                case 0: store.set(key, val); ref.set(key, val); break;
                case 1: (void)store.get(key); (void)ref.get(key); break;
                case 2: store.del(key); ref.del(key); break;
            }
        }
        // Store is destroyed here (simulates restart).
    }

    // Phase 2: reopen and verify model matches recovered state.
    {
        auto store = make_store(tf.wal);
        // Check that every key in the reference model is present in store.
        for (const auto& [k, e] : ref.raw()) {
            if (ref.exists(k)) {
                const auto sv = store.get(k);
                ASSERT_TRUE(sv.has_value());
                ASSERT_EQ(*sv, ref.get(k).value());
            } else {
                ASSERT_TRUE(!store.exists(k));
            }
        }

        // Phase 3: continue OPS_POST operations after recovery.
        for (int i = 0; i < OPS_POST; ++i) {
            const std::string key = "k" + std::to_string(key_dist(rng));
            const std::string val = "v" + std::to_string(val_dist(rng));
            switch (op_dist(rng)) {
                case 0: store.set(key, val); ref.set(key, val); break;
                case 1: break; // skip GET in this phase
                case 2: store.del(key); ref.del(key); break;
            }
        }
    }
}

// =============================================================================
// RZ3. State-machine with compaction mid-sequence: state preserved.
// =============================================================================
TEST(rz3_state_machine_with_compaction) {
    TempFiles tf("rz3");

    constexpr std::uint32_t SEED     = 0xDEADBEEF;
    constexpr int           OPS      = 400;
    constexpr int           KEY_SPACE = 20;
    constexpr int           COMPACT_AT = 200;

    std::mt19937 rng(SEED);
    std::uniform_int_distribution<int> op_dist(0, 2);
    std::uniform_int_distribution<int> key_dist(0, KEY_SPACE - 1);
    std::uniform_int_distribution<int> val_dist(0, 999);

    ReferenceModel ref;
    auto store = make_store(tf.wal);

    for (int i = 0; i < OPS; ++i) {
        if (i == COMPACT_AT) {
            store.compact();
            // State must still match after compaction.
            for (const auto& [k, e] : ref.raw()) {
                if (ref.exists(k)) {
                    const auto sv = store.get(k);
                    ASSERT_TRUE(sv.has_value());
                    ASSERT_EQ(*sv, ref.get(k).value());
                }
            }
        }

        const std::string key = "k" + std::to_string(key_dist(rng));
        const std::string val = "v" + std::to_string(val_dist(rng));
        switch (op_dist(rng)) {
            case 0: store.set(key, val); ref.set(key, val); break;
            case 1: break; // skip GET for simplicity
            case 2: store.del(key); ref.del(key); break;
        }
    }

    // Final state: all reference keys must match store.
    for (const auto& [k, e] : ref.raw()) {
        if (ref.exists(k)) {
            ASSERT_TRUE(store.exists(k));
            ASSERT_EQ(*store.get(k), ref.get(k).value());
        }
    }
}

// =============================================================================
// Test runner
// =============================================================================

int main() {
    const auto& tests = test_registry();
    int passed = 0, failed = 0;
    std::cout << "\nForgeKV Stage 13 — Randomized State-Machine Tests\n";
    std::cout << std::string(51, '=') << "\n\n";
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
    std::cout << "\n" << std::string(51, '=') << "\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed"
              << " (total: " << (passed + failed) << ")\n\n";
    return (failed == 0) ? 0 : 1;
}
