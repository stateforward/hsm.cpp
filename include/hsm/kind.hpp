#ifndef HSM_KIND_HPP
#define HSM_KIND_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "detail/fixed_string.hpp"
#include "detail/hash.hpp"
namespace hsm {

using kind_t = std::uint64_t;

namespace kind {
constexpr std::size_t length = 64;
constexpr std::size_t id_length = 16;
constexpr std::size_t depth_max = length / id_length;
constexpr kind_t id_mask = (1ULL << id_length) - 1;

constexpr kind_t id(kind_t kind) { return static_cast<kind_t>((kind)&id_mask); }

constexpr auto bases(kind_t kind) {
  auto bases = std::array<kind_t, depth_max>{};
  for (std::size_t i = 1; i < depth_max; i++) {
    bases[i - 1] = (kind >> (id_length * i)) & id_mask;
  }
  return bases;
}

}  // namespace kind

template <typename T>
requires std::is_convertible_v<T, std::uint64_t>
constexpr kind_t to_kind_t(T t) {
  return static_cast<kind_t>(t);
}




template <typename... TBases>
constexpr kind_t make_kind(kind_t id = 0, TBases... bases) {
  // static_assert((std::is_convertible_v<TBases, kind_t> && ...),
  //              "bases must be convertible to kind_t");

  std::array<kind_t, kind::depth_max * kind::depth_max> kind_ids{};
  auto bases_ids = std::array<kind_t, sizeof...(bases)>{static_cast<kind_t>(bases)...};
  std::size_t index = 0;
  kind_t kind_id = (id + 1) & kind::id_mask;
  for (std::size_t i = 0; i < sizeof...(bases); i++) {
    auto base = bases_ids[i];
    for (std::size_t j = 0; j < kind::depth_max; j++) {
      kind_t base_id = (base >> (kind::id_length * j)) & kind::id_mask;
      if (base_id == 0) {
        break;
      }
      bool exists = false;
      for (std::size_t k = 0; k < index; k++) {
        if (kind_ids[k] == base_id) {
          exists = true;
          break;
        }
      }
      if (!exists) {
        kind_ids[index] = base_id;
        index++;
        kind_id |= base_id << (kind::id_length * index);
      }
    }
  }

  return kind_id;
}

template <>
constexpr kind_t make_kind(kind_t id) {
  return (id + 1) & kind::id_mask;
}

template<typename... TBases>
constexpr kind_t make_kind(std::string_view name, TBases... bases) {
  return make_kind(detail::fnv1a_64(name), std::forward<TBases>(bases)...);
}

template <typename... TBases>
constexpr kind_t MakeKind(kind_t id = 0, TBases... bases) {
  return make_kind(id, std::forward<TBases>(bases)...);
}

template<typename... TBases>
constexpr kind_t MakeKind(std::string_view name, TBases... bases) {
  return make_kind(name, std::forward<TBases>(bases)...);
}

template <typename Tkind, typename TBase, typename... TBases>
constexpr bool is_kind(Tkind kind, TBase base, TBases... bases) {
  return is_kind(kind, base) || is_kind(kind, bases...);
}

template <typename Tkind, typename TBase>
constexpr bool is_kind(Tkind kind, TBase base) {
  kind_t base_id = kind::id(static_cast<kind_t>(base));
  for (std::size_t i = 0; i < kind::depth_max; i++) {
    kind_t current_id =
        kind::id(static_cast<kind_t>(kind) >> (kind::id_length * i));
    if (current_id == base_id) {
      return true;
    } else if (current_id == 0) {
      break;
    }
  }
  return false;
}

template <typename Tkind, typename TBase, typename... TBases>
constexpr bool IsKind(Tkind kind, TBase base, TBases... bases) {
  return is_kind(kind, base, bases...);
}

constexpr kind_t base(kind_t kind) { return kind >> kind::id_length; }

enum class Kind : kind_t {
  Null = 0,
  Element = make_kind(1),
  Vertex = make_kind(3, Element),
  State = make_kind(4, Vertex),
  Namespace = make_kind(5, State),
  FinalState = make_kind(6, State),
  Transition = make_kind(7, Element),
  Pseudostate = make_kind(8, Vertex),
  Initial = make_kind(9, Pseudostate),
  Choice = make_kind(10, Pseudostate),
  Behavior = make_kind(11, Element),
  StateMachine = make_kind(14, State),
  External = make_kind(15, Transition),
  Self = make_kind(16, Transition),
  Internal = make_kind(17, Transition),
  Local = make_kind(18, Transition),
  Constraint = make_kind(19, Element),
  Event = make_kind(20, Element),
  CompletionEvent = make_kind(21, Event),
  TimeEvent = make_kind(22, Event),
  InitialEvent = make_kind(23, Event),
  AnyEvent = make_kind(24, Event),
  Entry = make_kind(25, Behavior),
  Exit = make_kind(26, Behavior),
  Activity = make_kind(27, Behavior),
  Guard = make_kind(28, Constraint),
  Effect = make_kind(29, Behavior),
  ChangeEvent = make_kind(30, Event),
  CallEvent = make_kind(31, Event),
};

}  // namespace hsm

#endif // HSM_KIND_HPP
