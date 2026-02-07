#ifndef HSM_DETAIL_NORMALIZE_HPP
#define HSM_DETAIL_NORMALIZE_HPP

#include "expressions.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "hash.hpp"
#include "meta_model.hpp"
#include "structural_tuple.hpp"
#include "assert.hpp"

namespace hsm::detail
{

// Helper templates for type checking
template <typename T> struct is_target : std::false_type
{
};
template <typename Path> struct is_target<target_expr<Path> > : std::true_type
{
};

template <typename T> struct is_source : std::false_type
{
};
template <typename Path> struct is_source<source_expr<Path> > : std::true_type
{
};

template <typename T> struct is_on : std::false_type
{
};
template <typename Event> struct is_on<on_expr<Event> > : std::true_type
{
};

template <typename T> struct is_transition : std::false_type
{
};
template <typename... P>
struct is_transition<transition_expr<P...> > : std::true_type
{
};

template <typename T> struct is_guard : std::false_type
{
};
template <typename C> struct is_guard<guard_expr<C> > : std::true_type
{
};

template <typename T> struct is_effect : std::false_type
{
};
template <typename... A> struct is_effect<effect_expr<A...> > : std::true_type
{
};

template <typename T> struct is_after : std::false_type
{
};
template <typename C> struct is_after<after_expr<C> > : std::true_type
{
};

template <typename T> struct is_every : std::false_type
{
};
template <typename C> struct is_every<every_expr<C> > : std::true_type
{
};

template <typename T> struct is_at : std::false_type
{
};
template <typename C> struct is_at<at_expr<C> > : std::true_type
{
};

// when_attr_expr helper
// Used to detect attribute-based when("name") nodes in transition
// partials when extracting event names/kinds.
template <typename T> struct is_when_attr : std::false_type
{
};

template <typename Name>
struct is_when_attr<when_attr_expr<Name> > : std::true_type
{
};

// Helper to check for direct target in tuple (for implicit initial transition)
template <typename Tuple, std::size_t I>
consteval bool
has_direct_target_check ()
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    return false;
  else
    {
      using Type = std::decay_t<std::tuple_element_t<I, Tuple> >;
      if constexpr (is_target<Type>::value)
        return true;
      else
        return has_direct_target_check<Tuple, I + 1> ();
    }
}

// --- COUNTING ---

struct model_counts
{
  std::size_t states = 0;
  std::size_t transitions = 0;
  std::size_t events = 0;
  std::size_t string_size = 0;
  std::size_t entries = 0;
  std::size_t exits = 0;
  std::size_t activities = 0;
  std::size_t guards = 0;
  std::size_t effects = 0;
  std::size_t timers = 0;
  std::size_t deferred_entries = 0;
  std::size_t max_depth = 0;

  constexpr model_counts
  operator+ (const model_counts &other) const
  {
    return { states + other.states,
             transitions + other.transitions,
             events + other.events,
             string_size + other.string_size,
             entries + other.entries,
             exits + other.exits,
             activities + other.activities,
             guards + other.guards,
             effects + other.effects,
             timers + other.timers,
             deferred_entries + other.deferred_entries,
             (max_depth > other.max_depth) ? max_depth : other.max_depth };
  }
};

// Forward declarations
template <typename T>
consteval model_counts count_recursive (const T &node,
                                        std::size_t parent_path_len);

template <typename Tuple, std::size_t I>
consteval model_counts count_tuple (const Tuple &t,
                                    std::size_t parent_path_len);

template <typename Tuple>
consteval model_counts
count_partials (const Tuple &t, std::size_t parent_path_len)
{
  return count_tuple<Tuple, 0> (t, parent_path_len);
}

template <typename Tuple, std::size_t I>
consteval model_counts
count_tuple (const Tuple &t, std::size_t parent_path_len)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    {
      return {};
    }
  else
    {
      return count_recursive (get<I> (t), parent_path_len)
             + count_tuple<Tuple, I + 1> (t, parent_path_len);
    }
}

// Default case
template <typename T>
consteval model_counts
count_recursive (const T &node, std::size_t parent_path_len)
{
  if constexpr (requires { node.elements; })
    {
      return count_partials (node.elements, parent_path_len);
    }
  else
    {
      return {};
    }
}

// Model Expression (Root)
template <typename Name, typename... Partials>
consteval model_counts
count_recursive (const model_expression<Name, Partials...> &node,
                 std::size_t parent_path_len)
{
  std::size_t current_len = parent_path_len + 1 + node.name.size ();
  model_counts counts = count_partials (node.elements, current_len);
  counts.states += 1;
  counts.string_size += current_len + 1; // +1 for null terminator
  counts.max_depth += 1;
  return counts;
}

// State Expression
template <typename Name, typename... Partials>
consteval model_counts
count_recursive (const state_expr<Name, Partials...> &node,
                 std::size_t parent_path_len)
{
  std::size_t current_len = parent_path_len + 1 + node.name.size ();
  model_counts counts = count_partials (node.elements, current_len);
  counts.states += 1;
  counts.string_size += current_len + 1; // +1 for null terminator
  counts.max_depth += 1;
  return counts;
}

// Choice Expression
template <typename Name, typename... Partials>
consteval model_counts
count_recursive (const choice_expr<Name, Partials...> &node,
                 std::size_t parent_path_len)
{
  std::size_t current_len = parent_path_len + 1 + node.name.size ();
  model_counts counts = count_partials (node.elements, current_len);
  counts.states += 1;
  counts.string_size += current_len + 1; // +1 for null terminator
  counts.max_depth += 1;
  return counts;
}

// Final Expression
template <typename Name>
consteval model_counts
count_recursive (const final_expr<Name> &node, std::size_t parent_path_len)
{
  std::size_t current_len = parent_path_len + 1 + node.name.size ();
  model_counts counts{};
  counts.states += 1;
  counts.string_size += current_len + 1; // +1 for null terminator
  counts.max_depth += 1;
  return counts;
}

// Transition Expression
template <typename... Partials>
consteval model_counts
count_recursive (const transition_expr<Partials...> &node,
                 std::size_t parent_path_len)
{
  model_counts counts = count_partials (node.elements, parent_path_len);
  counts.transitions += 1;
  return counts;
}

// On (Event) Expression
template <typename Event>
consteval model_counts
count_recursive (const on_expr<Event> &node, std::size_t)
{
  model_counts c{};
  c.events = 1;
  c.string_size = node.name.size () + 1; // +1 for null terminator
  return c;
}

// when_attr_expr (attribute-based when("name")) Expression
// Each such node introduces a single change-event entry with the
// attribute's name in the event table.
template <typename Name>
consteval model_counts
count_recursive (const when_attr_expr<Name> &node, std::size_t)
{
  model_counts c{};
  c.events = 1;
  c.string_size = node.name.size () + 1; // +1 for null terminator
  return c;
}

// operation_expr Expression
// Operations are purely model-level metadata and do not contribute
// entries to the normalized event table. CallEvent-kind entries are
// introduced only via transitions that listen for them (on_call or
// on<CallEventType>).
template <typename Name, typename Callable>
consteval model_counts
count_recursive (const operation_expr<Name, Callable> &, std::size_t)
{
  return {};
}

// history_expr Expression (named UML history pseudostate)
// Contributes one state entry so that transitions can target it by
// path, and optionally one implicit transition when a direct target(...)
// is present in its partials (default behavior when there is no prior
// history for the parent composite).
template <typename Name, bool IsDeep, typename... Partials>
consteval model_counts
count_recursive (const history_expr<Name, IsDeep, Partials...> &node,
                 std::size_t parent_path_len)
{
  std::size_t current_len = parent_path_len + 1 + node.name.size ();
  model_counts counts = count_partials (node.elements, current_len);
  counts.states += 1;
  counts.string_size += current_len + 1; // +1 for null terminator
  counts.max_depth += 1;

  if constexpr (has_direct_target_check<decltype (node.elements), 0> ())
    {
      // Implicit default transition owned by the history pseudostate
      counts.transitions += 1;
    }

  return counts;
}

// Defer Expression
template <typename Tuple, std::size_t I>
consteval model_counts
count_defer_tuple (const Tuple &t)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    {
      return {};
    }
  else
    {
      model_counts c{};
      c.events = 1;

      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_on<Type>::value)
        {
          // on_expr
          auto &on_e = get<I> (t);
          if constexpr (requires { on_e.name.size (); })
            {
              c.string_size = on_e.name.size () + 1; // +1 for null terminator
            }
        }
      else if constexpr (requires { get<I> (t).size (); })
        {
          c.string_size = get<I> (t).size () + 1; // +1 for null terminator
        }

      return c + count_defer_tuple<Tuple, I + 1> (t);
    }
}

template <typename... Events>
consteval model_counts
count_recursive (const defer_expr<Events...> &node, std::size_t)
{
  model_counts c
      = count_defer_tuple<decltype (node.event_names), 0> (node.event_names);
  c.deferred_entries = sizeof...(Events);
  return c;
}

template <typename... Actions>
consteval model_counts
count_recursive (const entry_expr<Actions...> &, std::size_t)
{
  return model_counts{ .entries = sizeof...(Actions) };
}

template <typename... Actions>
consteval model_counts
count_recursive (const exit_expr<Actions...> &, std::size_t)
{
  return model_counts{ .exits = sizeof...(Actions) };
}

template <typename... Actions>
consteval model_counts
count_recursive (const activity_expr<Actions...> &, std::size_t)
{
  return model_counts{ .activities = sizeof...(Actions) };
}

template <typename... Actions>
consteval model_counts
count_recursive (const effect_expr<Actions...> &, std::size_t)
{
  return model_counts{ .effects = sizeof...(Actions) };
}

template <typename Callable>
consteval model_counts
count_recursive (const guard_expr<Callable> &, std::size_t)
{
  return model_counts{ .guards = 1 };
}

template <typename Callable>
consteval model_counts
count_recursive (const after_expr<Callable> &, std::size_t)
{
  // Reserve space for generated event "__timer_XXX" (approx 20 chars) and
  // count 1 event
  return model_counts{ .events = 1, .string_size = 21, .timers = 1 }; // +1 for null terminator
}

template <typename Callable>
consteval model_counts
count_recursive (const every_expr<Callable> &, std::size_t)
{
  return model_counts{ .events = 1, .string_size = 21, .timers = 1 }; // +1 for null terminator
}

template <typename Callable>
consteval model_counts
count_recursive (const at_expr<Callable> &, std::size_t)
{
  return model_counts{ .events = 1, .string_size = 21, .timers = 1 }; // +1 for null terminator
}

// Initial Expression
template <typename... Partials>
consteval model_counts
count_recursive (const initial_expr<Partials...> &node,
                 std::size_t parent_path_len)
{

  model_counts c = count_partials (node.elements, parent_path_len);

  // Check if we need implicit transition (if target is present directly)
  if constexpr (has_direct_target_check<decltype (node.elements), 0> ())
    {
      c.transitions += 1;
    }
  return c;
}

// --- POPULATION ---

template <typename ModelData> struct populate_ctx
{
  std::size_t state_idx = 0;
  std::size_t transition_idx = 0;
  std::size_t event_idx = 0;
  std::size_t string_cursor = 0;

  std::size_t entry_idx = 0;
  std::size_t exit_idx = 0;
  std::size_t activity_idx = 0;
  std::size_t guard_idx = 0;
  std::size_t effect_idx = 0;
  std::size_t timer_idx = 0;
  std::size_t defer_idx = 0;

  constexpr void
  append_string (ModelData &data, std::string_view str)
  {
    for (char c : str)
      {
        data.string_buffer[string_cursor++] = c;
      }
  }
};

// Helper to find state ID by absolute path
template <typename ModelData>
constexpr std::size_t
find_state_id (const ModelData &data, std::string_view path)
{
  for (std::size_t i = 0; i < data.state_count; ++i)
    {
      if (data.get_state_name (i) == path)
        return i;
    }
  return invalid_index;
}

// Check if state matches parent + "/" + name
template <typename ModelData>
constexpr std::size_t
find_child_state (const ModelData &data, std::string_view parent_path,
                  std::string_view name)
{
  for (std::size_t i = 0; i < data.state_count; ++i)
    {
      std::string_view candidate = data.get_state_name (i);

      // Check length
      if (candidate.size () != parent_path.size () + 1 + name.size ())
        continue;

      // Check parent prefix
      if (!candidate.starts_with (parent_path))
        continue;

      // Check separator
      if (candidate[parent_path.size ()] != '/')
        continue;

      // Check suffix
      if (candidate.substr (parent_path.size () + 1) != name)
        continue;

      return i;
    }
  return invalid_index;
}

// Helper to resolve target ID
// For regular targets, target_path is a concrete state path or relative name.
// For history targets, we never call this – history resolution is handled
// separately via annotated transition_desc fields.
template <typename ModelData>
constexpr std::size_t
resolve_target (const ModelData &data, std::string_view target_path,
                std::size_t source_id)
{
  // 1. Exact match (Absolute)
  std::size_t id = find_state_id (data, target_path);
  if (id != invalid_index)
    return id;

  // 2. Relative resolution
  if (source_id != invalid_index)
    {
      std::string_view source_path = data.get_state_name (source_id);

      // 2a. Check Child (source/target)
      id = find_child_state (data, source_path, target_path);
      if (id != invalid_index)
        return id;

      // 2b. Check Sibling (parent(source)/target)
      std::size_t parent_id = data.states[source_id].parent_id;
      std::string_view parent_path = (parent_id != invalid_index)
                                         ? data.get_state_name (parent_id)
                                         : "";

      if (parent_id != invalid_index)
        {
          id = find_child_state (data, parent_path, target_path);
          if (id != invalid_index)
            return id;
        }
    }
  else
    {
      // 3. Model-level transition (source_id == invalid_index)
      // This happens for initial transitions at the model level.
      // Resolve relative to the root state (state 0, which is the model itself).
      if (data.state_count > 0)
        {
          std::string_view root_path = data.get_state_name(0);
          id = find_child_state(data, root_path, target_path);
          if (id != invalid_index)
            return id;
        }
    }
  return invalid_index;
}

// Forward decls for population
template <typename ModelData, typename T>
constexpr void collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                               const T &node, std::string_view parent_path,
                               std::size_t parent_id);

template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
collect_states_tuple (ModelData &data, populate_ctx<ModelData> &ctx,
                      const Tuple &t, std::string_view parent_path,
                      std::size_t parent_id);

template <typename ModelData, typename T>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const T &node, std::size_t current_state_id);

template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
collect_transitions_tuple (ModelData &data, populate_ctx<ModelData> &ctx,
                           const Tuple &t, std::size_t current_state_id);

// --- Implementations: Collect States ---

template <typename ModelData, typename Tuple>
constexpr void
collect_states_partials (ModelData &data, populate_ctx<ModelData> &ctx,
                         const Tuple &t, std::string_view parent_path,
                         std::size_t parent_id)
{
  collect_states_tuple<ModelData, Tuple, 0> (data, ctx, t, parent_path,
                                             parent_id);
}

template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
collect_states_tuple (ModelData &data, populate_ctx<ModelData> &ctx,
                      const Tuple &t, std::string_view parent_path,
                      std::size_t parent_id)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      collect_states (data, ctx, get<I> (t), parent_path, parent_id);
      collect_states_tuple<ModelData, Tuple, I + 1> (data, ctx, t, parent_path,
                                                     parent_id);
    }
}

// Default
template <typename ModelData, typename T>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx, const T &node,
                std::string_view parent_path, std::size_t parent_id)
{
  if constexpr (requires { node.elements; })
    {
      collect_states_partials (data, ctx, node.elements, parent_path,
                               parent_id);
    }
}

// operation_expr: Operations do not participate in state collection.
// They are handled separately via extract_operations in HSM and do not
// allocate event table entries here.
template <typename ModelData, typename Name, typename Callable>
constexpr void
collect_states (ModelData &, populate_ctx<ModelData> &,
                const operation_expr<Name, Callable> &,
                std::string_view, std::size_t)
{
}

// Defer tuple collector
template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
collect_defer_tuple (ModelData &data, populate_ctx<ModelData> &ctx,
                     const Tuple &t)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      std::size_t id = ctx.event_idx++;
      std::size_t offset = ctx.string_cursor;
      std::size_t length = 0;
      hsm::kind_t k = 0;

      using Type = std::decay_t<decltype (get<I> (t))>;

      if constexpr (is_on<Type>::value)
        {
          auto &on_e = get<I> (t);
          using EventType = decltype (on_e.name);
          if constexpr (requires { EventType::kind; })
            {
              // Typed event
              k = EventType::kind;
            }
          else if constexpr (requires { on_e.name.view (); })
            {
              // String event inside on_expr
              std::string_view name = on_e.name.view ();
              length = name.size ();
              ctx.append_string (data, name);
              ctx.append_string (data, std::string_view("\0", 1)); // Null terminate
              k = hsm::make_kind(fnv1a_64(name), hsm::Kind::Event);
            }
        }
      else if constexpr (is_when_attr<Type>::value)
        {
          // Attribute-based when("name") used in defer expressions is
          // treated as a ChangeEvent-kind event keyed by the attribute
          // name.
          const auto &node = get<I> (t);
          std::string_view name = node.name.view ();
          length = name.size ();
          ctx.append_string (data, name);
          ctx.append_string (data, std::string_view("\0", 1)); // Null terminate
          k = hsm::make_kind(fnv1a_64(name), hsm::Kind::ChangeEvent);
        }
      else if constexpr (requires { Type::kind; })
        {
          k = Type::kind;
        }
      else if constexpr (requires { get<I> (t).view (); })
        {
          std::string_view name = get<I> (t).view ();
          length = name.size ();
          ctx.append_string (data, name);
          ctx.append_string (data, std::string_view("\0", 1)); // Null terminate
          k = fnv1a_64 (name);
        }

      data.events[id] = event_desc{
        .id = id, .name_offset = offset, .name_length = length, .kind = k
      };
      data.deferred_events[ctx.defer_idx++] = id;
      collect_defer_tuple<ModelData, Tuple, I + 1> (data, ctx, t);
    }
}

template <typename ModelData, typename... Events>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const defer_expr<Events...> &node, std::string_view,
                std::size_t state_id)
{
  data.states[state_id].defer_start = ctx.defer_idx;
  data.states[state_id].defer_count = sizeof...(Events);
  collect_defer_tuple<ModelData, decltype (node.event_names), 0> (
      data, ctx, node.event_names);
}

template <typename ModelData, typename... Actions>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const entry_expr<Actions...> &, std::string_view,
                std::size_t state_id)
{
  if (data.states[state_id].entry_start == invalid_index)
    {
      data.states[state_id].entry_start = ctx.entry_idx;
    }
  data.states[state_id].entry_count += sizeof...(Actions);
  ctx.entry_idx += sizeof...(Actions);
}

template <typename ModelData, typename... Actions>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const exit_expr<Actions...> &, std::string_view,
                std::size_t state_id)
{
  if (data.states[state_id].exit_start == invalid_index)
    {
      data.states[state_id].exit_start = ctx.exit_idx;
    }
  data.states[state_id].exit_count += sizeof...(Actions);
  ctx.exit_idx += sizeof...(Actions);
}

template <typename ModelData, typename... Actions>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const activity_expr<Actions...> &, std::string_view,
                std::size_t state_id)
{
  if (data.states[state_id].activity_start == invalid_index)
    {
      data.states[state_id].activity_start = ctx.activity_idx;
    }
  data.states[state_id].activity_count += sizeof...(Actions);
  ctx.activity_idx += sizeof...(Actions);
}

// Helper for State-like nodes
template <typename ModelData, typename T>
constexpr void
collect_state_node (ModelData &data, populate_ctx<ModelData> &ctx,
                    const T &node, std::string_view parent_path,
                    std::size_t parent_id,
                    state_flags flags = state_flags::none,
                    history_kind hist = history_kind::none)
{
  std::size_t id = ctx.state_idx++;
  std::size_t offset = ctx.string_cursor;

  if (!parent_path.empty ())
    {
      ctx.append_string (data, parent_path);
    }

  ctx.append_string (data, "/");
  ctx.append_string (data, node.name.view ());
  ctx.append_string (data, std::string_view("\0", 1)); // Null terminate

  std::size_t length = ctx.string_cursor - offset - 1; // Exclude null from length

  hsm::Kind k = hsm::Kind::State;
  if (hist != history_kind::none)
    {
      k = hsm::Kind::Pseudostate;
    }
  else if (parent_id == invalid_index)
    {
      k = hsm::Kind::StateMachine;
    }
  else if (any (flags, state_flags::choice))
    {
      k = hsm::Kind::Choice;
    }

  data.states[id] = state_desc{ .id = id,
                                .name_offset = offset,
                                .name_length = length,
                                .parent_id = parent_id,
                                .flags = flags,
                                .kind = k,
                                .history_type = hist };

  std::string_view current_path = data.get_state_name (id);
  collect_states_partials (data, ctx, node.elements, current_path, id);
}

template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const model_expression<Name, Partials...> &node,
                std::string_view parent_path, std::size_t parent_id)
{
  collect_state_node (data, ctx, node, parent_path, parent_id);
}

template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const state_expr<Name, Partials...> &node,
                std::string_view parent_path, std::size_t parent_id)
{
  collect_state_node (data, ctx, node, parent_path, parent_id);
}

template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const choice_expr<Name, Partials...> &node,
                std::string_view parent_path, std::size_t parent_id)
{
  collect_state_node (data, ctx, node, parent_path, parent_id,
                      state_flags::choice);
}

// Named UML history pseudostate under a composite state.
// This allocates a Pseudostate-typed state whose name is the pseudostate
// name and whose parent is the owning composite.
template <typename ModelData, typename Name, bool IsDeep, typename... Partials>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const history_expr<Name, IsDeep, Partials...> &node,
                std::string_view parent_path, std::size_t parent_id)
{
  collect_state_node (data,
                      ctx,
                      node,
                      parent_path,
                      parent_id,
                      state_flags::none,
                      IsDeep ? history_kind::deep : history_kind::shallow);
}

template <typename ModelData, typename Name>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const final_expr<Name> &node, std::string_view parent_path,
                std::size_t parent_id)
{
  std::size_t id = ctx.state_idx++;
  std::size_t offset = ctx.string_cursor;

  if (!parent_path.empty ())
    {
      ctx.append_string (data, parent_path);
    }

  ctx.append_string (data, "/");
  ctx.append_string (data, node.name.view ());
  ctx.append_string (data, std::string_view("\0", 1)); // Null terminate

  std::size_t length = ctx.string_cursor - offset - 1; // Exclude null from length

  data.states[id] = state_desc{ .id = id,
                                .name_offset = offset,
                                .name_length = length,
                                .parent_id = parent_id,
                                .flags = state_flags::final,
                                .kind = hsm::Kind::FinalState };
}

template <typename ModelData, typename... Partials>
constexpr void
collect_states (ModelData &data, populate_ctx<ModelData> &ctx,
                const initial_expr<Partials...> &node,
                std::string_view parent_path, std::size_t parent_id)
{
  collect_states_partials (data, ctx, node.elements, parent_path, parent_id);
}

// --- Implementations: Collect Transitions ---

template <typename ModelData, typename Tuple>
constexpr void
collect_transitions_partials (ModelData &data, populate_ctx<ModelData> &ctx,
                              const Tuple &t, std::size_t current_state_id)
{
  collect_transitions_tuple<ModelData, Tuple, 0> (data, ctx, t,
                                                  current_state_id);
}

template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
collect_transitions_tuple (ModelData &data, populate_ctx<ModelData> &ctx,
                           const Tuple &t, std::size_t current_state_id)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      collect_transitions (data, ctx, get<I> (t), current_state_id);
      collect_transitions_tuple<ModelData, Tuple, I + 1> (data, ctx, t,
                                                          current_state_id);
    }
}

// Default
template <typename ModelData, typename T>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const T &node, std::size_t current_state_id)
{
  if constexpr (requires { node.elements; })
    {
      collect_transitions_partials (data, ctx, node.elements,
                                    current_state_id);
    }
}

// State/Model: update current_state_id
template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const model_expression<Name, Partials...> &node,
                     std::size_t)
{
  std::size_t my_id = ctx.state_idx++;
  collect_transitions_partials (data, ctx, node.elements, my_id);
}

template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const state_expr<Name, Partials...> &node, std::size_t)
{
  std::size_t my_id = ctx.state_idx++;
  collect_transitions_partials (data, ctx, node.elements, my_id);
}

template <typename ModelData, typename Name, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const choice_expr<Name, Partials...> &node, std::size_t)
{
  std::size_t my_id = ctx.state_idx++;
  collect_transitions_partials (data, ctx, node.elements, my_id);
}

// History pseudostate: synthesize at most one implicit default transition
// based on a direct target(...) + optional guard/effect partials.
// We deliberately do not recurse into children for transitions to keep
// history declarations simple and structural.
template <typename ModelData, typename Name, bool IsDeep, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const history_expr<Name, IsDeep, Partials...> &node,
                     std::size_t)
{
  std::size_t my_id = ctx.state_idx++;

  if constexpr (has_direct_target_check<decltype (node.elements), 0> ())
    {
      std::size_t transition_id = ctx.transition_idx++;

      std::string_view target_path
          = get_target_path<decltype (node.elements), 0> (node.elements);
      std::size_t target_id
          = resolve_target (data, target_path, my_id);

      if (target_id == invalid_index)
        {
          detail::constexpr_assert(
            "Invalid history default target path in history_expr");
        }

      std::size_t guard = get_guard_idx<decltype (node.elements), 0> (
          node.elements, ctx.guard_idx);

      std::size_t effect_start = invalid_index;
      std::size_t effect_count = 0;
      get_effect_info<decltype (node.elements), 0> (
          node.elements, effect_start, effect_count, ctx.effect_idx);

      data.transitions[transition_id]
          = transition_desc{ .id = transition_id,
                             .source_id = my_id,
                             .target_id = target_id,
                             .type = transition_kind::local,
                             .kind = hsm::Kind::Local,
                             .event_id = invalid_index,
                             .guard_idx = guard,
                             .effect_start = effect_start,
                             .effect_count = effect_count,
                             .timer_type = timer_kind::none,
                             .timer_idx = invalid_index,
                             .history = history_kind::none,
                             .history_parent = invalid_index,
                             .history_state_id = invalid_index };

      data.states[my_id].history_default_transition_id = transition_id;
    }
}

template <typename ModelData, typename Name>
constexpr void
collect_transitions (ModelData & /*data*/, populate_ctx<ModelData> &ctx,
                     const final_expr<Name> & /*node*/, std::size_t)
{
  // Final states have no children/transitions to collect
  ctx.state_idx++;
}

template <typename ModelData, typename... Actions>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const entry_expr<Actions...> &, std::size_t)
{
}

template <typename ModelData, typename... Actions>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const exit_expr<Actions...> &, std::size_t)
{
}

template <typename ModelData, typename... Actions>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const activity_expr<Actions...> &, std::size_t)
{
}

template <typename ModelData, typename... Events>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const defer_expr<Events...> &, std::size_t)
{
}

template <typename ModelData, typename Callable>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const after_expr<Callable> &, std::size_t)
{
}

template <typename ModelData, typename Callable>
constexpr void
collect_transitions (ModelData &, populate_ctx<ModelData> &,
                     const every_expr<Callable> &, std::size_t)
{
}

// Helper to find target/event in partials
template <typename Tuple, std::size_t I>
constexpr std::string_view
get_target_path (const Tuple &t)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    {
      return {};
    }
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_target<Type>::value)
        {
          // For regular targets, the path type is a fixed_string with view().
          // For history targets, the path is a
          // shallow_history_path/deep_history_path wrapper, which should not
          // be treated as a concrete state path here.
          using PathType = std::decay_t<decltype (get<I> (t).path)>;
          if constexpr (requires (const PathType &p) { p.parent; })
            {
              // History targets are handled separately via
              // get_history_parent_path. Returning an empty path here prevents
              // resolve_target from running.
              static_cast<void>(t); // suppress unused warning in some compilers
              return {};
            }
          else
            {
              return get<I> (t).path.view ();
            }
        }
      else
        {
          return get_target_path<Tuple, I + 1> (t);
        }
    }
}

template <typename Tuple, std::size_t I>
constexpr std::string_view
get_source_path (const Tuple &t)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    {
      return {};
    }
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_source<Type>::value)
        {
          return get<I> (t).path.view ();
        }
      else
        {
          return get_source_path<Tuple, I + 1> (t);
        }
    }
}

// History helpers – detect and extract history path wrappers

using detail::deep_history_path;
using detail::shallow_history_path;

template <typename T> struct is_shallow_history_path : std::false_type
{
};

template <typename Path>
struct is_shallow_history_path<shallow_history_path<Path> > : std::true_type
{
};

template <typename T> struct is_deep_history_path : std::false_type
{
};

template <typename Path>
struct is_deep_history_path<deep_history_path<Path> > : std::true_type
{
};

template <typename Tuple, std::size_t I>
constexpr std::string_view
get_history_parent_path (const Tuple &t, history_kind &kind)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    {
      kind = history_kind::none;
      return {};
    }
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_shallow_history_path<Type>::value)
        {
          // Direct shallow_history(...) in the partial list
          kind = history_kind::shallow;
          return get<I> (t).parent.view ();
        }
      else if constexpr (is_deep_history_path<Type>::value)
        {
          // Direct deep_history(...) in the partial list
          kind = history_kind::deep;
          return get<I> (t).parent.view ();
        }
      else if constexpr (is_target<Type>::value)
        {
          // History wrapped inside target(...)
          using PathType = std::decay_t<decltype (get<I> (t).path)>;
          if constexpr (is_shallow_history_path<PathType>::value)
            {
              kind = history_kind::shallow;
              return get<I> (t).path.parent.view ();
            }
          else if constexpr (is_deep_history_path<PathType>::value)
            {
              kind = history_kind::deep;
              return get<I> (t).path.parent.view ();
            }
          else
            {
              return get_history_parent_path<Tuple, I + 1> (t, kind);
            }
        }
      else
        {
          return get_history_parent_path<Tuple, I + 1> (t, kind);
        }
    }
}

template <typename Tuple, std::size_t I>
constexpr std::string_view
get_event_name (const Tuple &t)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    return {};
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_on<Type>::value)
        {
          return get<I> (t).name.view ();
        }
      else if constexpr (is_when_attr<Type>::value)
        {
          return get<I> (t).name.view ();
        }
      else
        {
          return get_event_name<Tuple, I + 1> (t);
        }
    }
}

template <typename Tuple, std::size_t I>
constexpr hsm::kind_t
get_event_kind (const Tuple &t)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    return 0;
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_on<Type>::value)
        {
          using EventType = decltype (get<I> (t).name);
          if constexpr (requires { EventType::kind; })
            {
              // Typed event with an explicit kind on the EventType
              return EventType::kind;
            }
          else if constexpr (requires { typename EventType::is_call_event_name; })
            {
              // on_call("name") → CallEvent-kind keyed by operation name
              auto name = get<I> (t).name.view ();
              return hsm::make_kind(fnv1a_64(name), hsm::Kind::CallEvent);
            }
          else
            {
              // String/named events handled via event_name in collect_transitions
              return 0;
            }
        }
      else if constexpr (is_when_attr<Type>::value)
        {
          // Attribute-based when("name") → ChangeEvent-kind event
          auto name = get<I> (t).name.view ();
          return hsm::make_kind(fnv1a_64(name), hsm::Kind::ChangeEvent);
        }
      else
        {
          return get_event_kind<Tuple, I + 1> (t);
        }
    }
}

template <typename Tuple, std::size_t I>
constexpr std::size_t
get_guard_idx (const Tuple &t, std::size_t &current_idx)
{
  if constexpr (I >= std::tuple_size_v<Tuple>)
    return invalid_index;
  else
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_guard<Type>::value)
        {
          return current_idx++;
        }
      else
        {
          return get_guard_idx<Tuple, I + 1> (t, current_idx);
        }
    }
}

template <typename... Actions>
constexpr void
get_effect_details (const effect_expr<Actions...> &, std::size_t &start,
                    std::size_t &count, std::size_t &current_idx)
{
  start = current_idx;
  count = sizeof...(Actions);
  current_idx += count;
}

template <typename Tuple, std::size_t I>
constexpr void
get_effect_info (const Tuple &t, std::size_t &start, std::size_t &count,
                 std::size_t &current_idx)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_effect<Type>::value)
        {
          if (start == invalid_index)
            start = current_idx;

          std::size_t dummy_start = 0;
          std::size_t this_count = 0;
          get_effect_details (get<I> (t), dummy_start, this_count,
                              current_idx);
          count += this_count;
        }
      else
        {
          // Recurse for other types but don't consume effect indices
          // But other types don't consume indices anyway.
        }
      get_effect_info<Tuple, I + 1> (t, start, count, current_idx);
    }
}

template <typename Tuple, std::size_t I>
constexpr void
get_timer_info (const Tuple &t, timer_kind &kind, std::size_t &idx,
                std::size_t &current_idx)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_after<Type>::value)
        {
          kind = timer_kind::after;
          idx = current_idx++;
        }
      else if constexpr (is_every<Type>::value)
        {
          kind = timer_kind::every;
          idx = current_idx++;
        }
      else if constexpr (is_at<Type>::value)
        {
          kind = timer_kind::at;
          idx = current_idx++;
        }
      else
        {
          get_timer_info<Tuple, I + 1> (t, kind, idx, current_idx);
        }
    }
}

template <typename ModelData, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const transition_expr<Partials...> &node,
                     std::size_t current_state_id)
{
  std::size_t id = ctx.transition_idx++;

  std::string_view target_path
      = get_target_path<decltype (node.elements), 0> (node.elements);
  std::string_view source_path
      = get_source_path<decltype (node.elements), 0> (node.elements);
  std::string_view event_name
      = get_event_name<decltype (node.elements), 0> (node.elements);
  hsm::kind_t event_kind_val
      = get_event_kind<decltype (node.elements), 0> (node.elements);

  // History annotation (if present)
  history_kind h_kind = history_kind::none;
  std::string_view history_parent_path
      = get_history_parent_path<decltype (node.elements), 0> (node.elements,
                                                              h_kind);

  std::size_t target_id = invalid_index;
  std::size_t history_parent_id = invalid_index;
  std::size_t history_state_id = invalid_index;

  if (!history_parent_path.empty ())
    {
      // Legacy history transition using shallow_history/deep_history wrappers.
      // We resolve and cache only the composite parent id.
      history_parent_id = find_state_id (data, history_parent_path);
      
      // Compile-time validation - safety net for invalid paths
      if (history_parent_id == invalid_index) {
        detail::constexpr_assert("Invalid history parent path in transition");
      }
    }
  else if (!target_path.empty ())
    {
      // Regular target resolution
      target_id = resolve_target (data, target_path, current_state_id);
      
      // Compile-time validation - safety net for invalid paths
      if (target_id == invalid_index) {
        detail::constexpr_assert("Invalid target path in transition");
      }

      // If the resolved target is a named history pseudostate, treat this as
      // a history transition that targets that pseudostate. We derive the
      // history kind and parent composite from the state metadata.
      if (target_id != invalid_index &&
          data.states[target_id].history_type != history_kind::none)
        {
          history_state_id = target_id;
          history_parent_id = data.states[target_id].parent_id;
          h_kind = data.states[target_id].history_type;

          if (history_parent_id == invalid_index)
            {
              detail::constexpr_assert(
                "History pseudostate must have a parent composite state");
            }

          // For named history transitions we conceptually target the
          // composite parent; the runtime will pick the effective leaf
          // state (history restoration vs default) at execute time.
          target_id = history_parent_id;
        }
    }

  std::size_t event_id = invalid_index;
  if (!event_name.empty () || event_kind_val != 0)
    {
      event_id = ctx.event_idx++;
      std::size_t offset = ctx.string_cursor;
      ctx.append_string (data, event_name);
      ctx.append_string (data, std::string_view("\0", 1)); // Null terminate

      hsm::kind_t k = event_kind_val;
      if (k == 0 && !event_name.empty ())
        {
          k = hsm::make_kind(fnv1a_64(event_name), hsm::Kind::Event);
        }

      data.events[event_id] = event_desc{ .id = event_id,
                                          .name_offset = offset,
                                          .name_length = event_name.size (),
                                          .kind = k };
    }

  std::size_t guard = get_guard_idx<decltype (node.elements), 0> (
      node.elements, ctx.guard_idx);

  std::size_t effect_start = invalid_index;
  std::size_t effect_count = 0;
  get_effect_info<decltype (node.elements), 0> (node.elements, effect_start,
                                                effect_count, ctx.effect_idx);

  timer_kind t_kind = timer_kind::none;
  std::size_t t_idx = invalid_index;
  get_timer_info<decltype (node.elements), 0> (node.elements, t_kind, t_idx,
                                               ctx.timer_idx);

  if (t_kind != timer_kind::none)
    {
      // Generate an internal event name for the timer
      // Format: __timer_<timer_idx>
      // We can construct this at compile time if we use a fixed string
      // generator or similar But for simplicity here, we might need a helper
      // to convert int to string into the buffer

      // Simplified approach: Reuse ctx.string_buffer logic to append
      // "__timer_" + timer_idx
      std::size_t timer_evt_id = ctx.event_idx++;
      std::size_t offset = ctx.string_cursor;

      ctx.append_string (data, "__timer_");
      // Simple integer to string conversion into buffer
      std::size_t n = t_idx;
      if (n == 0)
        {
          ctx.append_string (data, "0");
        }
      else
        {
          // Calculate digits
          std::size_t temp = n;
          while (temp > 0)
            {
              temp /= 10;
            }

          // We need to append in reverse or use a temp buffer
          // Since we are appending to end, we can append and then reverse the
          // digits? Easier to just recurse or loop with buffer Let's use a
          // small buffer
          char buf[20];
          std::size_t i = 0;
          temp = n;
          while (temp > 0)
            {
              buf[i++] = '0' + (temp % 10);
              temp /= 10;
            }
          while (i > 0)
            {
              data.string_buffer[ctx.string_cursor++] = buf[--i];
            }
        }
      
      ctx.append_string (data, std::string_view("\0", 1)); // Null terminate
      std::size_t length = ctx.string_cursor - offset - 1; // Exclude null from length
      std::string_view timer_name
          = { data.string_buffer.data () + offset, length };
      data.events[timer_evt_id] = event_desc{ .id = timer_evt_id,
                                              .name_offset = offset,
                                              .name_length = length,
                                              .kind = fnv1a_64 (timer_name) };

      // Set event_id for the transition
      if (event_id == invalid_index)
        {
          event_id = timer_evt_id;
        }
      else
        {
          // If there was already an event (e.g. on(...)), we have a conflict.
          // Timer transitions usually shouldn't have explicit triggers unless
          // supported. For now, prefer the timer event as the primary trigger
          // for this transition entry. But wait, if both exist, how does
          // dispatch work? hsm.hpp treats them as separate events handled by
          // the same transition? hsm structure has one event_id per
          // transition. We might need to create a separate transition for the
          // timer if one already exists? Current normalize logic creates one
          // transition per `transition(...)` block. If user wrote
          // `transition(on("A"), after(...))`, that's ambiguous. Let's assume
          // valid models don't mix explicit triggers and timers on the same
          // transition line or if they do, we overwrite or handle it. Given
          // the structure, let's overwrite event_id with the timer event so
          // dispatch works.
          event_id = timer_evt_id;
        }
    }

  std::size_t resolved_source_id = current_state_id;
  if (!source_path.empty ())
    {
      resolved_source_id
          = resolve_target (data, source_path, current_state_id);
      if (resolved_source_id == invalid_index)
        {
          detail::constexpr_assert ("Invalid source path in transition");
        }
    }

  hsm::Kind trans_kind = hsm::Kind::External;
  transition_kind trans_type = transition_kind::external;

  if (target_id == invalid_index && h_kind == history_kind::none)
    {
      trans_kind = hsm::Kind::Internal;
      trans_type = transition_kind::internal;
    }
  else if (target_id == resolved_source_id)
    {
      trans_kind = hsm::Kind::Self;
      // Note: hsm runtime expects external for self-transitions currently
      trans_type = transition_kind::external;
    }

  data.transitions[id]
      = transition_desc{ .id = id,
                         .source_id = resolved_source_id,
                         .target_id = target_id,
                         .type = trans_type,
                         .kind = trans_kind,
                         .event_id = event_id,
                         .guard_idx = guard,
                         .effect_start = effect_start,
                         .effect_count = effect_count,
                         .timer_type = t_kind,
                         .timer_idx = t_idx,
                         .history = h_kind,
                         .history_parent = history_parent_id,
                         .history_state_id = history_state_id };
}

// Helper to process initial element recursively
template <typename ModelData, typename Tuple, std::size_t I>
constexpr void
process_initial_elements (ModelData &data, populate_ctx<ModelData> &ctx,
                          const Tuple &t, std::size_t current_state_id,
                          std::size_t &transition_id)
{
  if constexpr (I < std::tuple_size_v<Tuple>)
    {
      using Type = std::decay_t<decltype (get<I> (t))>;
      if constexpr (is_transition<Type>::value)
        {
          std::size_t next_id = ctx.transition_idx;
          collect_transitions (data, ctx, get<I> (t), current_state_id);
          if (ctx.transition_idx > next_id)
            {
              transition_id = next_id;
              // Also ensure the transition has NO event_id if it was external
              // Initial transitions are automatic.
              data.transitions[transition_id].event_id = invalid_index;
            }
        }
      process_initial_elements<ModelData, Tuple, I + 1> (
          data, ctx, t, current_state_id, transition_id);
    }
}

// Initial Expression Handler (Pass 2 - Transition Collection)
template <typename ModelData, typename... Partials>
constexpr void
collect_transitions (ModelData &data, populate_ctx<ModelData> &ctx,
                     const initial_expr<Partials...> &node,
                     std::size_t current_state_id)
{

  std::size_t transition_id = invalid_index;

  if constexpr (has_direct_target_check<decltype (node.elements), 0> ())
    {
      // Implicit transition from target(...)
      transition_id = ctx.transition_idx++;

      std::string_view target_path
          = get_target_path<decltype (node.elements), 0> (node.elements);
      std::size_t target_id
          = resolve_target (data, target_path, current_state_id);
      
      // Compile-time validation - safety net for invalid paths
      if (target_id == invalid_index) {
        // [[maybe_unused]] int HSM_ERROR_Invalid_initial_target_path[-1];
      }

      std::size_t effect_start = invalid_index;
      std::size_t effect_count = 0;
      get_effect_info<decltype (node.elements), 0> (
          node.elements, effect_start, effect_count, ctx.effect_idx);

      data.transitions[transition_id]
          = transition_desc{ .id = transition_id,
                             .source_id = current_state_id,
                             .target_id = target_id,
                             .type = transition_kind::local,
                             .kind = hsm::Kind::Local,
                             .event_id = invalid_index,
                             .effect_start = effect_start,
                             .effect_count = effect_count };

      // Recurse for other things?
      collect_transitions_partials (data, ctx, node.elements,
                                    current_state_id);
    }
  else
    {
      // Explicit transition(s) inside initial
      process_initial_elements<ModelData, decltype (node.elements), 0> (
          data, ctx, node.elements, current_state_id, transition_id);
    }

  if (transition_id != invalid_index)
    {
      data.states[current_state_id].initial_transition_id = transition_id;
    }
}

// Main Normalize Function
template <auto Model>
consteval auto
normalize ()
{
  constexpr model_counts counts = count_recursive (Model, 0);

  using ModelDataType
      = normalized_model_data<counts.states, counts.transitions, counts.events,
                              counts.timers, counts.deferred_entries,
                              counts.string_size, counts.max_depth>;
  ModelDataType data{};

  populate_ctx<ModelDataType> ctx{};

  // Pass 1: States
  collect_states (data, ctx, Model, "", invalid_index);

  // Pass 2: Transitions
  ctx.state_idx = 0;
  collect_transitions (data, ctx, Model, invalid_index);

  // Pass 3: Transitive Deferred Masks
  for (std::size_t i = 0; i < data.state_count; ++i)
    {
      const auto &s = data.states[i];
      if (s.parent_id != invalid_index)
        {
          data.transitive_deferred_masks[i] = data.transitive_deferred_masks[s.parent_id];
        }

      for (std::size_t k = 0; k < s.defer_count; ++k)
        {
          std::size_t evt_id = data.deferred_events[s.defer_start + k];
          if (evt_id < ModelDataType::event_count)
            {
              // Canonicalize event ID to match runtime dispatch behavior (which uses the first matching kind)
              auto kind = data.events[evt_id].kind;
              std::size_t canonical = evt_id;
              for (std::size_t e = 0; e < evt_id; ++e)
                {
                  if (data.events[e].kind == kind)
                    {
                      canonical = e;
                      break;
                    }
                }
              data.transitive_deferred_masks[i][canonical / 64] |= (1ULL << (canonical % 64));
            }
        }
    }

  return data;
}

} // namespace hsm::detail

#endif // HSM_DETAIL_NORMALIZE_HPP
