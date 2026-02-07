#ifndef HSM_DETAIL_EXTRACT_EVENTS_HPP
#define HSM_DETAIL_EXTRACT_EVENTS_HPP

#include <tuple>
#include <type_traits>
#include <utility>

#include "expressions.hpp"
#include "normalize.hpp"

namespace hsm::detail {

// --- Types ---
using hsm::Event;
using hsm::TimeEvent;
using PlaceholderEvent = hsm::Event<0>;

// Traits are available from normalize.hpp (included above)

// --- Pass 1: Deferred Events ---

template <typename T>
constexpr auto extract_deferred_from_node(const T& node);

template <typename Tuple, std::size_t... Is>
constexpr auto extract_deferred_from_tuple_impl(const Tuple& t, std::index_sequence<Is...>) {
    return std::tuple_cat(extract_deferred_from_node(get<Is>(t))...);
}

template <typename Tuple>
constexpr auto extract_deferred_from_tuple(const Tuple& t) {
    return extract_deferred_from_tuple_impl(t, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tuple, std::size_t... Is>
constexpr auto extract_deferred_events_from_defer_expr_impl(const Tuple& t, std::index_sequence<Is...>) {
    return std::tuple_cat([](const auto& item) { // item is dummy, we use type
        using Type = std::decay_t<decltype(item)>;
        if constexpr (is_on<Type>::value) {
            using EventType = decltype(std::declval<Type>().name);
            if constexpr (requires { EventType::kind; }) {
                return std::tuple<EventType>{};
            } else if constexpr (requires { EventType{}.view(); }) {
                // String event (fixed_string): extract as named_event<Name>
                return std::tuple<named_event<EventType{}>>{};
            } else {
                return std::tuple<PlaceholderEvent>{};
            }
        } else if constexpr (requires { typename Type::type; }) {
            return std::tuple<typename Type::type>{};
        } else if constexpr (requires { Type::kind; }) {
            return std::tuple<Type>{};
        } else if constexpr (requires { Type{}.view(); }) {
            // String literal (fixed_string): extract as named_event<Name>
            return std::tuple<named_event<Type{}>>{};
        } else {
            // Unknown
            return std::tuple<PlaceholderEvent>{};
        }
    }(get<Is>(t))...);
}

template <typename... Events>
constexpr auto extract_deferred_from_node(const defer_expr<Events...>& node) {
    return extract_deferred_events_from_defer_expr_impl(node.event_names, std::make_index_sequence<sizeof...(Events)>{});
}

// Recursive cases
template <typename Name, typename... Partials>
constexpr auto extract_deferred_from_node(const model_expression<Name, Partials...>& node) { return extract_deferred_from_tuple(node.elements); }
template <typename Name, typename... Partials>
constexpr auto extract_deferred_from_node(const state_expr<Name, Partials...>& node) { return extract_deferred_from_tuple(node.elements); }
template <typename Name, typename... Partials>
constexpr auto extract_deferred_from_node(const choice_expr<Name, Partials...>& node) { return extract_deferred_from_tuple(node.elements); }
template <typename... Partials>
constexpr auto extract_deferred_from_node(const initial_expr<Partials...>& node) { return extract_deferred_from_tuple(node.elements); }

// Terminal cases (return empty)
template <typename T> constexpr auto extract_deferred_from_node(const T&) { return std::tuple<>{}; }


// --- Pass 2: Transitions ---

template <typename T>
constexpr auto extract_transitions_from_node(const T& node);

template <typename Tuple, std::size_t... Is>
constexpr auto extract_transitions_from_tuple_impl(const Tuple& t, std::index_sequence<Is...>) {
    return std::tuple_cat(extract_transitions_from_node(get<Is>(t))...);
}

template <typename Tuple>
constexpr auto extract_transitions_from_tuple(const Tuple& t) {
    return extract_transitions_from_tuple_impl(t, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

// Helper to extract event type from on_expr - value-based version for string events
template <typename OnExpr, const OnExpr* = nullptr>
struct extract_on_type_base {
    using NameType = decltype(std::declval<OnExpr>().name);

    template <typename T, typename = void>
    struct impl {
        using type = std::tuple<PlaceholderEvent>;
    };

    template <typename T>
    struct impl<T, std::void_t<typename T::type>> {
        using type = std::tuple<typename T::type>;
    };

    // Fallback for T::kind but no T::type (legacy/robustness)
    template <typename T>
    struct impl<T, std::enable_if_t<!requires{ typename T::type; } && requires{ T::kind; }>> {
        using type = std::tuple<T>;
    };

    template <typename T>
    struct impl<T, std::enable_if_t<is_when_attr<T>::value>> {
        using type = std::tuple<PlaceholderEvent>;
    };

    // For string events (has .view() but no .type or .kind), default to placeholder
    // The value-based version below will handle these properly
    template <typename T>
    struct impl<T, std::enable_if_t<
        !requires{ typename T::type; } &&
        !requires{ T::kind; } &&
        !is_when_attr<T>::value &&
        requires{ T{}.view(); }
    >> {
        using type = std::tuple<PlaceholderEvent>;
    };

    using type = typename impl<NameType>::type;
};

// Type alias for backward compatibility (type-only version)
template <typename OnExpr>
using extract_on_type = extract_on_type_base<OnExpr>;

// Value-based extraction for constexpr on_expr with string events
// This computes Event<kind> using the actual string value, not a default-constructed type
template <auto OnExprValue>
struct extract_on_value {
    using OnExpr = std::decay_t<decltype(OnExprValue)>;
    using NameType = decltype(OnExprValue.name);

    static constexpr auto extract() {
        if constexpr (requires { NameType::kind; }) {
            // Typed event with explicit kind
            return std::tuple<NameType>{};
        } else if constexpr (requires { typename NameType::type; }) {
            // Has nested type
            return std::tuple<typename NameType::type>{};
        } else if constexpr (requires { OnExprValue.name.view(); }) {
            // String event - compute kind from actual value
            constexpr auto hash = fnv1a_64(OnExprValue.name.view());
            constexpr auto kind = hsm::make_kind(hash, hsm::Kind::Event);
            return std::tuple<hsm::Event<kind>>{};
        } else {
            return std::tuple<PlaceholderEvent>{};
        }
    }

    using type = decltype(extract());
};

// Helper to check is_on safely
template <typename Tuple, std::size_t I, typename = void>
struct is_element_on : std::false_type {};

template <typename Tuple, std::size_t I>
struct is_element_on<Tuple, I, std::enable_if_t<(I < std::tuple_size_v<Tuple>)>> {
    using Element = std::decay_t<std::tuple_element_t<I, Tuple>>;
    static constexpr bool value = is_on<Element>::value || is_when_attr<Element>::value;
};

// Recursive search for event type in tuple
template <typename Tuple, std::size_t I, typename Enable = void>
struct find_event_type_struct {
    // Recursion
    using type = typename find_event_type_struct<Tuple, I + 1>::type;
    static constexpr auto value = type{};
};

// Base case: End of tuple
template <typename Tuple, std::size_t I>
struct find_event_type_struct<Tuple, I, std::enable_if_t<(I >= std::tuple_size_v<Tuple>)>> {
    using type = std::tuple<>;
    static constexpr auto value = type{};
};

// Match case: is_on
template <typename Tuple, std::size_t I>
struct find_event_type_struct<Tuple, I, std::enable_if_t<is_element_on<Tuple, I>::value>> {
    using Element = std::decay_t<std::tuple_element_t<I, Tuple>>;
    using type = typename extract_on_type<Element>::type;
    static constexpr auto value = type{};
};

// Helper safely check timer
template <typename Tuple, std::size_t I, typename = void>
struct is_element_timer : std::false_type {};

template <typename Tuple, std::size_t I>
struct is_element_timer<Tuple, I, std::enable_if_t<(I < std::tuple_size_v<Tuple>)>> {
    using Type = std::decay_t<std::tuple_element_t<I, Tuple>>;
    static constexpr bool value = is_after<Type>::value || is_every<Type>::value || is_at<Type>::value;
};

template <typename Tuple, std::size_t I, typename Enable = void>
struct has_timer_struct {
    // Recursion
    static constexpr bool value = has_timer_struct<Tuple, I + 1>::value;
};

// Base case
template <typename Tuple, std::size_t I>
struct has_timer_struct<Tuple, I, std::enable_if_t<(I >= std::tuple_size_v<Tuple>)>> {
    static constexpr bool value = false;
};

// Match case
template <typename Tuple, std::size_t I>
struct has_timer_struct<Tuple, I, std::enable_if_t<is_element_timer<Tuple, I>::value>> {
    static constexpr bool value = true;
};

// Value-based event extraction from a transition - uses NTTP to preserve string values
template <auto TransitionValue>
constexpr auto extract_event_from_transition_value() {
    using Tuple = std::decay_t<decltype(TransitionValue.elements)>;

    // Find the on_expr element and extract its event type
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        auto extract_one = []<std::size_t I>() {
            using Element = std::decay_t<std::tuple_element_t<I, Tuple>>;
            if constexpr (is_on<Element>::value || is_when_attr<Element>::value) {
                constexpr auto& elem = get<I>(TransitionValue.elements);
                using NameType = std::decay_t<decltype(elem.name)>;

                if constexpr (requires { NameType::kind; }) {
                    return std::tuple<NameType>{};
                } else if constexpr (requires { typename NameType::type; }) {
                    return std::tuple<typename NameType::type>{};
                } else if constexpr (requires { elem.name.view(); }) {
                    // String event - compute kind from actual NTTP value
                    constexpr auto hash = fnv1a_64(elem.name.view());
                    constexpr auto kind = hsm::make_kind(hash, hsm::Kind::Event);
                    return std::tuple<hsm::Event<kind>>{};
                } else {
                    return std::tuple<PlaceholderEvent>{};
                }
            } else {
                return std::tuple<>{};
            }
        };
        return std::tuple_cat(extract_one.template operator()<Is>()...);
    }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename... Partials>
constexpr auto extract_transitions_from_node(const transition_expr<Partials...>& node) {
    using Tuple = std::decay_t<decltype(node.elements)>;

    // Use type-based extraction (fallback - string events get PlaceholderEvent)
    constexpr auto event_tuple = find_event_type_struct<Tuple, 0>::value;

    // Check for timer
    constexpr bool has_timer = has_timer_struct<Tuple, 0>::value;

    if constexpr (has_timer) {
        return std::tuple_cat(event_tuple, std::tuple<TimeEvent>{});
    } else {
        return event_tuple;
    }
}

// Recursive cases
template <typename Name, typename... Partials>
constexpr auto extract_transitions_from_node(const model_expression<Name, Partials...>& node) { return extract_transitions_from_tuple(node.elements); }
template <typename Name, typename... Partials>
constexpr auto extract_transitions_from_node(const state_expr<Name, Partials...>& node) { return extract_transitions_from_tuple(node.elements); }
template <typename Name, typename... Partials>
constexpr auto extract_transitions_from_node(const choice_expr<Name, Partials...>& node) { return extract_transitions_from_tuple(node.elements); }
template <typename... Partials>
constexpr auto extract_transitions_from_node(const initial_expr<Partials...>& node) { return extract_transitions_from_tuple(node.elements); }

// Terminal cases (return empty)
template <typename T> constexpr auto extract_transitions_from_node(const T&) { return std::tuple<>{}; }


// --- Main ---

template <typename Model>
constexpr auto extract_all_events(const Model& model) {
    return std::tuple_cat(extract_deferred_from_node(model), extract_transitions_from_node(model));
}

// --- NTTP-based extraction for string events ---
// These versions take the model as NTTP so string values are preserved at compile time.

template <auto Node, std::size_t I = 0>
constexpr auto extract_transitions_nttp() {
    using NodeType = std::decay_t<decltype(Node)>;

    if constexpr (is_transition<NodeType>::value) {
        // This is a transition - extract its event
        using Tuple = std::decay_t<decltype(Node.elements)>;
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            auto extract_one = []<std::size_t J>() {
                using Element = std::decay_t<std::tuple_element_t<J, Tuple>>;
                if constexpr (is_on<Element>::value) {
                    constexpr auto& elem = get<J>(Node.elements);
                    using NameType = std::decay_t<decltype(elem.name)>;

                    // Check ::type FIRST (handles typed_kind<T> -> T)
                    if constexpr (requires { typename NameType::type; }) {
                        return std::tuple<typename NameType::type>{};
                    } else if constexpr (requires { NameType::kind; }) {
                        // Direct event type with kind
                        return std::tuple<NameType>{};
                    } else if constexpr (requires { typename NameType::is_call_event_name; }) {
                        // on_call("name") - CallEvent-kind event
                        constexpr auto hash = fnv1a_64(elem.name.view());
                        constexpr auto kind = hsm::make_kind(hash, hsm::Kind::CallEvent);
                        return std::tuple<hsm::Event<kind>>{};
                    } else if constexpr (requires { elem.name.view(); }) {
                        // String event - use actual value from NTTP
                        constexpr auto hash = fnv1a_64(elem.name.view());
                        constexpr auto kind = hsm::make_kind(hash, hsm::Kind::Event);
                        return std::tuple<hsm::Event<kind>>{};
                    } else {
                        return std::tuple<PlaceholderEvent>{};
                    }
                } else if constexpr (is_when_attr<Element>::value) {
                    // Attribute change event (when("attr_name"))
                    constexpr auto& elem = get<J>(Node.elements);
                    constexpr auto hash = fnv1a_64(elem.name.view());
                    constexpr auto kind = hsm::make_kind(hash, hsm::Kind::ChangeEvent);
                    return std::tuple<hsm::Event<kind>>{};
                } else {
                    return std::tuple<>{};
                }
            };
            // Also check for timer
            constexpr bool has_timer = has_timer_struct<Tuple, 0>::value;
            if constexpr (has_timer) {
                return std::tuple_cat(extract_one.template operator()<Is>()..., std::tuple<TimeEvent>{});
            } else {
                return std::tuple_cat(extract_one.template operator()<Is>()...);
            }
        }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    } else if constexpr (requires { Node.elements; }) {
        // Has nested elements - recurse
        using Tuple = std::decay_t<decltype(Node.elements)>;
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return std::tuple_cat(extract_transitions_nttp<get<Is>(Node.elements)>()...);
        }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
    } else {
        return std::tuple<>{};
    }
}

// Main NTTP-based extraction entry point
template <auto Model>
constexpr auto extract_all_events_nttp() {
    return std::tuple_cat(
        extract_deferred_from_node(Model),  // Deferred can use type-based (typed events only)
        extract_transitions_nttp<Model>()   // Transitions use NTTP-based for string values
    );
}

} // namespace hsm::detail

#endif // HSM_DETAIL_EXTRACT_EVENTS_HPP
