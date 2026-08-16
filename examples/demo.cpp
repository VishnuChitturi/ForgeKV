// =============================================================================
// ForgeKV — Stage 1: Demo
// =============================================================================
//
// A simple runnable example demonstrating all four KeyValueStore operations.
// Run this after building to see the store in action.
// =============================================================================

#include "forgekv/kv_store.h"

#include <iostream>
#include <string>

// Helper: print the result of a GET, showing presence or absence clearly.
static void print_get(const std::string& key,
                      const std::optional<std::string>& result) {
    if (result.has_value()) {
        std::cout << "  GET  \"" << key << "\" => \"" << *result << "\"\n";
    } else {
        std::cout << "  GET  \"" << key << "\" => (not found)\n";
    }
}

int main() {
    std::cout << "\n=== ForgeKV Stage 1 Demo ===\n\n";

    forgekv::KeyValueStore store;

    // ------------------------------------------------------------------
    // SET: store some key-value pairs
    // ------------------------------------------------------------------
    std::cout << "--- SET operations ---\n";
    store.set("name", "Vishnu");
    std::cout << "  SET  \"name\" = \"Vishnu\"\n";

    store.set("age", "21");
    std::cout << "  SET  \"age\"  = \"21\"\n";

    store.set("city", "Bengaluru");
    std::cout << "  SET  \"city\" = \"Bengaluru\"\n";

    std::cout << "  store size: " << store.size() << "\n\n";

    // ------------------------------------------------------------------
    // GET: retrieve stored values
    // ------------------------------------------------------------------
    std::cout << "--- GET operations ---\n";
    print_get("name", store.get("name"));
    print_get("age",  store.get("age"));
    print_get("city", store.get("city"));
    print_get("country", store.get("country")); // not set — returns nullopt
    std::cout << "\n";

    // ------------------------------------------------------------------
    // EXISTS: check presence without retrieving
    // ------------------------------------------------------------------
    std::cout << "--- EXISTS operations ---\n";
    std::cout << "  EXISTS \"name\"    => " << (store.exists("name")    ? "true" : "false") << "\n";
    std::cout << "  EXISTS \"country\" => " << (store.exists("country") ? "true" : "false") << "\n\n";

    // ------------------------------------------------------------------
    // UPDATE: SET on an existing key overwrites the value
    // ------------------------------------------------------------------
    std::cout << "--- UPDATE (SET on existing key) ---\n";
    std::cout << "  Before: ";
    print_get("name", store.get("name"));
    store.set("name", "Vishnu Kumar");
    std::cout << "  SET  \"name\" = \"Vishnu Kumar\"\n";
    std::cout << "  After:  ";
    print_get("name", store.get("name"));
    std::cout << "  store size: " << store.size() << " (unchanged — upsert)\n\n";

    // ------------------------------------------------------------------
    // DELETE: remove a key
    // ------------------------------------------------------------------
    std::cout << "--- DELETE operations ---\n";
    bool removed = store.del("age");
    std::cout << "  DEL  \"age\"     => " << (removed ? "removed" : "key not found") << "\n";
    print_get("age", store.get("age"));

    bool ghost = store.del("country"); // never existed
    std::cout << "  DEL  \"country\" => " << (ghost ? "removed" : "key not found") << "\n\n";

    // ------------------------------------------------------------------
    // Final state
    // ------------------------------------------------------------------
    std::cout << "--- Final store state ---\n";
    std::cout << "  size: " << store.size() << "\n";
    print_get("name", store.get("name"));
    print_get("city", store.get("city"));

    // ------------------------------------------------------------------
    // CLEAR: wipe everything
    // ------------------------------------------------------------------
    std::cout << "\n--- CLEAR ---\n";
    store.clear();
    std::cout << "  store.clear() called\n";
    std::cout << "  size after clear: " << store.size() << "\n";
    std::cout << "  empty: " << (store.empty() ? "true" : "false") << "\n";

    std::cout << "\n=== Demo complete ===\n\n";
    return 0;
}
