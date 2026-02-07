#ifndef HSM_DETAIL_EXPRESSIONS_HPP
#define HSM_DETAIL_EXPRESSIONS_HPP

#include <tuple>
#include <utility>

#include "../kind.hpp"
#include "structural_tuple.hpp"

namespace hsm::detail {

template <typename Name, typename... Partials>
struct model_expression {
  using name_type = Name;
  using partials_type = structural_tuple<Partials...>;

  Name name{};
  structural_tuple<Partials...> elements;
};

template <Kind K>
struct kind_expr {
  static constexpr kind_t kind = static_cast<kind_t>(K);
};

template <typename Name, typename... Partials>
struct state_expr : kind_expr<Kind::State> {
  Name name;
  structural_tuple<Partials...> elements;
};

template <typename... Partials>
struct transition_expr : kind_expr<Kind::Transition> {
  structural_tuple<Partials...> elements;
};

template <typename... Partials>
struct initial_expr : kind_expr<Kind::Initial> {
  structural_tuple<Partials...> elements;
};

template <typename Name, typename... Partials>
struct choice_expr : kind_expr<Kind::Choice> {
  Name name;
  structural_tuple<Partials...> elements;
};

// Named UML history pseudostate declaration. This is a state-like node that
// is normalized into a Pseudostate vertex with optional default transition
// semantics (when no prior history exists for its parent composite).
//
// IsDeep == true  -> deep history
// IsDeep == false -> shallow history
//
// The default behavior (if any) is expressed via target/guard/effect partials
// attached directly to this node.
template <typename Name, bool IsDeep, typename... Partials>
struct history_expr : kind_expr<Kind::Pseudostate> {
  Name name;
  structural_tuple<Partials...> elements;
  static constexpr bool is_deep = IsDeep;
};

template <typename Name>
struct final_expr : kind_expr<Kind::FinalState> {
  Name name;
};

template <typename... Actions>
struct entry_expr : kind_expr<Kind::Entry> {
  structural_tuple<Actions...> actions;
};

template <typename... Actions>
struct exit_expr : kind_expr<Kind::Exit> {
  structural_tuple<Actions...> actions;
};

template <typename... Actions>
struct effect_expr : kind_expr<Kind::Effect> {
  structural_tuple<Actions...> actions;
};

template <typename... Actions>
struct activity_expr : kind_expr<Kind::Activity> {
  structural_tuple<Actions...> actions;
};

template <typename Callable>
struct guard_expr : kind_expr<Kind::Guard> {
  Callable predicate;
};

template <typename Event>
struct on_expr : kind_expr<Kind::Event> {
  Event name;
};

template <typename Path>
struct target_expr {
  Path path;
};

// History target path wrappers. These do not introduce new states in the
// normalized model – they annotate transitions so the runtime can resolve
// history according to UML 2.5 (shallow / deep).

template <typename Path>
struct shallow_history_path {
  Path parent;  // absolute path of composite state owning the history
                // pseudostate
};

template <typename Path>
struct deep_history_path {
  Path parent;  // absolute path of composite state owning the history
                // pseudostate
};

template <typename Path>
struct source_expr {
  Path path;
};

template <typename... Events>
struct defer_expr {
  structural_tuple<Events...> event_names;
};

template <typename Callable>
struct after_expr {
  Callable duration;
};

template <typename Callable>
struct every_expr {
  Callable duration;
};

template <typename Callable>
struct at_expr {
  Callable time_point;
};

// Attribute declaration at the model level. Attributes do not
// participate in the normalized state/transition structure; they
// are collected separately into attribute metadata.
template <typename Name, typename T, bool HasDefault>
struct attribute_expr;

// No-default variant: only the name participates in the structural
// model; the value type T is carried only via the template parameter.
// This keeps models that declare attributes for non-structural types
// (e.g., std::string, std::vector) usable as non-type template
// parameters.
template <typename Name, typename T>
struct attribute_expr<Name, T, false> {
  Name name;
};

// Defaulted variant: stores both the name and the default value so
// that HSM instances can be initialized from the DSL.
template <typename Name, typename T>
struct attribute_expr<Name, T, true> {
  Name name;
  T default_value;
};

// Model-level operation declaration. Operations are named callables
// that can be invoked via HSM::call<"name">(args...). They do not
// contribute states or transitions directly; instead they introduce
// CallEvent-kind entries into the model's event table during
// normalization.
template <typename Name, typename Callable>
struct operation_expr {
  Name name;
  Callable callable;
};

// Lightweight tags used by the DSL to reference model-level operations
// from behaviors (entry/exit/effect/activity) and guards by *name*.
//
// These tags do not carry runtime payload; they only encode the
// compile-time name type so that HSM can resolve the corresponding
// operation descriptor from operation_tuple and re-use the existing
// behavior invocation matrix.

template <typename Name>
struct operation_action {
  using name_type = Name;
  Name name;
};

template <typename Name>
struct operation_guard {
  using name_type = Name;
  Name name;
};

// Attribute-based change trigger used by when("name") in the DSL.
// This appears as a transition partial and is interpreted by
// normalization as an event whose name and kind are derived from
// the attribute name.
template <typename Name>
struct when_attr_expr {
  Name name;
};

// Traits for recognizing operation_action / operation_guard tags.

template <typename T> struct is_operation_action : std::false_type {};

template <typename Name>
struct is_operation_action<operation_action<Name> > : std::true_type {};

template <typename T>
inline constexpr bool is_operation_action_v
  = is_operation_action<std::remove_cvref_t<T> >::value;


template <typename T> struct is_operation_guard : std::false_type {};

template <typename Name>
struct is_operation_guard<operation_guard<Name> > : std::true_type {};

template <typename T>
inline constexpr bool is_operation_guard_v
  = is_operation_guard<std::remove_cvref_t<T> >::value;

}  // namespace hsm::detail

#endif // HSM_DETAIL_EXPRESSIONS_HPP
