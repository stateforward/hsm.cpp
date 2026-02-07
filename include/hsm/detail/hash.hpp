#ifndef HSM_DETAIL_HASH_HPP
#define HSM_DETAIL_HASH_HPP

#include <cstdint>
#include <string_view>

namespace hsm::detail {

constexpr std::uint64_t fnv1a_64(std::string_view s) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char c : s) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace hsm::detail

#endif // HSM_DETAIL_HASH_HPP
