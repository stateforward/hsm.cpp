#ifndef HSM_DETAIL_FIXED_STRING_HPP
#define HSM_DETAIL_FIXED_STRING_HPP

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace hsm::detail {

template <std::size_t N>
struct fixed_string {
  std::array<char, N> value{};

  constexpr fixed_string() noexcept = default;

  constexpr fixed_string(const char (&str)[N]) noexcept {
    for (std::size_t i = 0; i < N; ++i) {
      value[i] = str[i];
    }
  }

  constexpr fixed_string(const std::array<char, N>& arr) noexcept
      : value(arr) {}

  [[nodiscard]] constexpr std::size_t size() const noexcept {
    // Return length excluding null terminator if present at end
    // But for fixed_string<N> from literal "foo", N is 4 (including \0).
    // We want size() to be 3.
    std::size_t len = 0;
    while (len < N && value[len] != '\0') {
      ++len;
    }
    return len;
  }

  [[nodiscard]] constexpr const char* data() const noexcept {
    return value.data();
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept {
    return std::string_view(value.data(), size());
  }

  [[nodiscard]] constexpr char operator[](std::size_t idx) const noexcept {
    return idx < N ? value[idx] : '\0';
  }

  template <std::size_t M>
  friend constexpr bool operator==(const fixed_string& lhs,
                                   const fixed_string<M>& rhs) noexcept {
    const auto lhs_size = lhs.size();
    if (lhs_size != rhs.size()) {
      return false;
    }
    for (std::size_t i = 0; i < lhs_size; ++i) {
      if (lhs.value[i] != rhs.value[i]) {
        return false;
      }
    }
    return true;
  }

  template <std::size_t M>
  friend constexpr bool operator!=(const fixed_string& lhs,
                                   const fixed_string<M>& rhs) noexcept {
    return !(lhs == rhs);
  }
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

template <std::size_t N>
constexpr auto make_fixed_string(const char (&literal)[N]) {
  return fixed_string<N>(literal);
}

template <typename T>
struct is_fixed_string : std::false_type {};

template <std::size_t N>
struct is_fixed_string<fixed_string<N>> : std::true_type {};

template <typename T>
inline constexpr bool is_fixed_string_v =
    is_fixed_string<std::remove_cvref_t<T>>::value;

namespace fs_detail {

template <typename Array, std::size_t Length>
consteval auto make_fixed_string_from_array(
    const Array& source, std::integral_constant<std::size_t, Length>) {
  constexpr std::size_t source_extent =
      std::tuple_size_v<std::remove_cvref_t<Array>>;
  static_assert(Length + 1 <= source_extent, "length must be <= source size");
  std::array<char, Length + 1> buffer{};
  for (std::size_t i = 0; i < Length; ++i) {
    buffer[i] = source[i];
  }
  buffer[Length] = '\0';
  return fixed_string<Length + 1>(buffer);
}

}  // namespace fs_detail

template <typename T>
concept fixed_string_literal = is_fixed_string_v<T>;

template <auto Value>
concept fixed_string_constant =
    is_fixed_string_v<std::remove_cvref_t<decltype(Value)>>;

}  // namespace hsm::detail

#endif // HSM_DETAIL_FIXED_STRING_HPP
