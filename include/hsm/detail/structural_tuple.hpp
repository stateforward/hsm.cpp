#ifndef HSM_DETAIL_STRUCTURAL_TUPLE_HPP
#define HSM_DETAIL_STRUCTURAL_TUPLE_HPP

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace hsm::detail {

template <typename... Ts>
struct structural_tuple;

template <>
struct structural_tuple<> {
  constexpr bool operator==(const structural_tuple&) const = default;
};

template <typename Head, typename... Tail>
struct structural_tuple<Head, Tail...> {
  Head head;
  structural_tuple<Tail...> tail;

  constexpr bool operator==(const structural_tuple&) const = default;
};

template <typename... Ts>
structural_tuple(Ts...) -> structural_tuple<Ts...>;

namespace tuple_detail {

template <std::size_t I, typename Tuple>
struct tuple_element;

template <std::size_t I, typename Head, typename... Tail>
struct tuple_element<I, structural_tuple<Head, Tail...>>
    : tuple_element<I - 1, structural_tuple<Tail...>> {};

template <typename Head, typename... Tail>
struct tuple_element<0, structural_tuple<Head, Tail...>> {
  using type = Head;
};

template <std::size_t I, typename Tuple>
struct get_impl;

template <std::size_t I, typename Head, typename... Tail>
struct get_impl<I, structural_tuple<Head, Tail...>> {
  static constexpr const auto& value(const structural_tuple<Head, Tail...>& t) {
    return get_impl<I - 1, structural_tuple<Tail...>>::value(t.tail);
  }
};

template <typename Head, typename... Tail>
struct get_impl<0, structural_tuple<Head, Tail...>> {
  static constexpr const Head& value(const structural_tuple<Head, Tail...>& t) {
    return t.head;
  }
};

}  // namespace tuple_detail

template <std::size_t I, typename... Ts>
constexpr const auto& get(const structural_tuple<Ts...>& t) {
  return tuple_detail::get_impl<I, structural_tuple<Ts...>>::value(t);
}

// Helper to make recursive construction easier

// Base case must be declared first
constexpr structural_tuple<> make_recursive_tuple() { return {}; }

template <typename Head, typename... Tail>
constexpr structural_tuple<Head, Tail...> make_recursive_tuple(Head h,
                                                               Tail... t) {
  return {h, make_recursive_tuple(t...)};
}

template <typename... Ts>
constexpr auto make_structural_tuple(Ts... args) {
  return make_recursive_tuple(args...);
}

template <typename... Ts, std::size_t... Is>
constexpr auto to_std_tuple_impl(const structural_tuple<Ts...>& st, std::index_sequence<Is...>) {
    return std::make_tuple(get<Is>(st)...);
}

template <typename... Ts>
constexpr auto to_std_tuple(const structural_tuple<Ts...>& st) {
    return to_std_tuple_impl(st, std::make_index_sequence<sizeof...(Ts)>{});
}

}  // namespace hsm::detail

namespace std {
template <typename... Ts>
struct tuple_size<hsm::detail::structural_tuple<Ts...>>
    : integral_constant<size_t, sizeof...(Ts)> {};

template <size_t I, typename... Ts>
struct tuple_element<I, hsm::detail::structural_tuple<Ts...>> {
  using type = typename hsm::detail::tuple_detail::tuple_element<
      I, hsm::detail::structural_tuple<Ts...>>::type;
};
}  // namespace std

#endif // HSM_DETAIL_STRUCTURAL_TUPLE_HPP
