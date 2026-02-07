#ifndef HSM_DETAIL_TYPE_LIST_HPP
#define HSM_DETAIL_TYPE_LIST_HPP

#include <cstddef>
#include <type_traits>

namespace hsm::detail
{

template <typename... Ts> struct type_list
{
};

template <typename List> struct type_list_size;

template <typename... Ts>
struct type_list_size<type_list<Ts...> >
    : std::integral_constant<std::size_t, sizeof...(Ts)>
{
};

template <typename List>
inline constexpr std::size_t type_list_size_v = type_list_size<List>::value;

template <typename List, typename T> struct type_list_push_back;

template <typename... Ts, typename T>
struct type_list_push_back<type_list<Ts...>, T>
{
  using type = type_list<Ts..., T>;
};

template <typename List, typename T>
using type_list_push_back_t = typename type_list_push_back<List, T>::type;

template <typename List, typename T> struct type_list_push_front;

template <typename... Ts, typename T>
struct type_list_push_front<type_list<Ts...>, T>
{
  using type = type_list<T, Ts...>;
};

template <typename List, typename T>
using type_list_push_front_t = typename type_list_push_front<List, T>::type;

template <typename... Lists> struct type_list_concat;

template <> struct type_list_concat<>
{
  using type = type_list<>;
};

template <typename... Ts> struct type_list_concat<type_list<Ts...> >
{
  using type = type_list<Ts...>;
};

template <typename... Ts, typename... Us, typename... Rest>
struct type_list_concat<type_list<Ts...>, type_list<Us...>, Rest...>
{
  using type =
      typename type_list_concat<type_list<Ts..., Us...>, Rest...>::type;
};

template <typename... Lists>
using type_list_concat_t = typename type_list_concat<Lists...>::type;

template <typename List, template <typename> class Predicate>
struct type_list_find;

template <template <typename> class Predicate>
struct type_list_find<type_list<>, Predicate>
{
  static constexpr bool value = false;
};

template <typename Head, typename... Tail, template <typename> class Predicate>
struct type_list_find<type_list<Head, Tail...>, Predicate>
{
  static constexpr bool value
      = Predicate<Head>::value
        || type_list_find<type_list<Tail...>, Predicate>::value;
};

template <typename List, typename T> struct type_list_contains;

template <typename T>
struct type_list_contains<type_list<>, T> : std::false_type
{
};

template <typename Head, typename... Tail, typename T>
struct type_list_contains<type_list<Head, Tail...>, T>
    : std::conditional_t<std::is_same_v<Head, T>, std::true_type,
                         type_list_contains<type_list<Tail...>, T> >
{
};

template <typename List, typename T>
inline constexpr bool type_list_contains_v
    = type_list_contains<List, T>::value;

template <typename List, typename Result = type_list<> >
struct type_list_unique;

template <typename Result> struct type_list_unique<type_list<>, Result>
{
  using type = Result;
};

template <typename Head, typename... Tail, typename Result>
struct type_list_unique<type_list<Head, Tail...>, Result>
{
  using next_result
      = std::conditional_t<type_list_contains_v<Result, Head>, Result,
                           type_list_push_back_t<Result, Head> >;
  using type =
      typename type_list_unique<type_list<Tail...>, next_result>::type;
};

template <typename List>
using type_list_unique_t = typename type_list_unique<List>::type;

template <typename List, typename T, std::size_t Index = 0>
struct type_list_index_of;

template <typename T, std::size_t Index>
struct type_list_index_of<type_list<>, T, Index>
    : std::integral_constant<std::size_t, static_cast<std::size_t> (-1)>
{
};

template <typename Head, typename... Tail, typename T, std::size_t Index>
struct type_list_index_of<type_list<Head, Tail...>, T, Index>
    : std::conditional_t<std::is_same_v<Head, T>,
                         std::integral_constant<std::size_t, Index>,
                         type_list_index_of<type_list<Tail...>, T, Index + 1> >
{
};

template <typename List, typename T>
inline constexpr std::size_t type_list_index_of_v
    = type_list_index_of<List, T>::value;

template <typename List, template <typename> class Transform>
struct type_list_transform;

template <template <typename> class Transform>
struct type_list_transform<type_list<>, Transform>
{
  using type = type_list<>;
};

template <typename Head, typename... Tail, template <typename> class Transform>
struct type_list_transform<type_list<Head, Tail...>, Transform>
{
  using type = type_list_push_front_t<
      typename type_list_transform<type_list<Tail...>, Transform>::type,
      typename Transform<Head>::type>;
};

template <typename List, template <typename> class Transform>
using type_list_transform_t =
    typename type_list_transform<List, Transform>::type;

} // namespace hsm::detail

#endif // HSM_DETAIL_TYPE_LIST_HPP
