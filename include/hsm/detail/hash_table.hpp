#pragma once

#include <array>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace hsm::detail {

template <typename Key, typename Value>
struct map_entry {
    Key key{};
    Value value{};
    bool occupied = false;
};

// Compile-time open-addressing hash map
template <typename Key, typename Value, std::size_t Capacity>
struct fixed_map {
    using entry_type = map_entry<Key, Value>;
    std::array<entry_type, Capacity> table{};

    constexpr bool insert(Key k, Value v) {
        // Simple hash: assume Key is castable to size_t or has hash
        std::size_t h;
        if constexpr (std::is_enum_v<Key>) h = static_cast<std::size_t>(k);
        else h = static_cast<std::size_t>(k);

        for (std::size_t i = 0; i < Capacity; ++i) {
            std::size_t idx = (h + i) % Capacity;
            if (!table[idx].occupied) {
                table[idx] = {k, v, true};
                return true;
            }
            if (table[idx].key == k) {
                // Key already exists.
                // For this use case, we just overwrite (or ignore)
                return false; 
            }
        }
        return false; // Full
    }

    constexpr const Value* find(Key k) const {
        std::size_t h;
        if constexpr (std::is_enum_v<Key>) h = static_cast<std::size_t>(k);
        else h = static_cast<std::size_t>(k);

        for (std::size_t i = 0; i < Capacity; ++i) {
            std::size_t idx = (h + i) % Capacity;
            if (!table[idx].occupied) return nullptr;
            if (table[idx].key == k) return &table[idx].value;
        }
        return nullptr;
    }
    
    // Helper to get with default
    constexpr Value get(Key k, Value def) const {
        const Value* v = find(k);
        return v ? *v : def;
    }
};

} // namespace hsm::detail
