#include "forgekv/kv_store.h"

#include <iostream>
#include <string>

int main() {
    forgekv::KeyValueStore store;

    std::string key;
    std::string value;

    std::cout << "ForgeKV Manual Test\n";
    std::cout << "-------------------\n";

    std::cout << "Enter key: ";
    std::getline(std::cin, key);

    std::cout << "Enter value: ";
    std::getline(std::cin, value);

    store.set(key, value);

    std::cout << "\nStored!\n";

    auto result = store.get(key);

    if (result.has_value()) {
        std::cout << "GET \"" << key << "\" => \""
                  << *result << "\"\n";
    } else {
        std::cout << "Key not found\n";
    }

    return 0;
}