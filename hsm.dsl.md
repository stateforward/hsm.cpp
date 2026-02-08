# HSM DSL — Domain-Specific Language Reference

Complete reference for the Domain-Specific Language of the hierarchical state machine library.

All DSL functions are **namespace/module-level functions** (not methods on objects), enabling model construction and validation at compile time.

> **Notation:** `dsl.FunctionName()` represents a function in the `dsl` namespace/module. These are free functions, not instance methods.

---

## Table of Contents

1. [Model Definition](#model-definition)
2. [State Declaration](#state-declaration)
3. [Pseudostates](#pseudostates)
4. [Transitions](#transitions)
5. [Event Triggers](#event-triggers)
6. [Timing Events](#timing-events)
7. [Transition Targets & Routing](#transition-targets--routing)
8. [State Behaviors](#state-behaviors)
9. [Guards & Deferral](#guards--deferral)
10. [Model Metadata](#model-metadata)
11. [Group Operations](#group-operations)
12. [Type Utilities](#type-utilities)
13. [Runtime Attribute Access](#runtime-attribute-access)
14. [Runtime Constants](#runtime-constants)

---

## Model Definition

### `dsl.Define(name, partials...)`

Declares a hierarchical state machine model with a name and zero or more child elements.

**Parameters:**
- `name` — Model name. Cannot contain the character `'/'`.
- `partials...` — Zero or more state/initial/transition/operation/attribute/group declarations

**Constraints:**
- Model names must not contain `'/'`
- Compile-time function

**Description:**
The top-level DSL entry point for constructing a state machine. Accepts a name and any combination of states, initial transitions, attributes, and operations to build the model structure.

---

## State Declaration

### `dsl.State(name, partials...)`

Declares a composite or basic state within the state machine hierarchy.

**Parameters:**
- `name` — State name. Cannot contain `'/'`.
- `partials...` — Entry/exit/activity actions, transitions, nested states

**Constraints:**
- State names must not contain `'/'`
- Compile-time function

**Description:**
Defines a named state in the hierarchy. Can contain nested substates, transitions, and behavioral actions.

### `dsl.Final(name)`

Declares a UML final state — an absorbing state with no outgoing transitions.

**Parameters:**
- `name` — Final state name. Cannot contain `'/'`.

**Constraints:**
- Final state names must not contain `'/'`
- Compile-time function

**Description:**
Represents a terminal state indicating the completion of a region or the entire state machine.

---

## Pseudostates

### `dsl.ShallowHistory(name, partials...)`

Declares a named shallow history pseudostate that remembers the most recent direct child of its parent composite state.

**Parameters:**
- `name` — History pseudostate name. Cannot contain `'/'`.
- `partials...` — Target, guard, and/or effect for the default transition (required)

**Constraints:**
- History names must not contain `'/'`
- Must have at least one partial (typically a target)
- Compile-time function

**Description:**
Upon entry to a history pseudostate, the machine transitions to the most recently active direct child of the parent state, or to a default target if no history exists.

### `dsl.DeepHistory(name, partials...)`

Declares a named deep history pseudostate that recursively remembers the deepest active leaf state.

**Parameters:**
- `name` — History pseudostate name. Cannot contain `'/'`.
- `partials...` — Target, guard, and/or effect for the default transition (required)

**Constraints:**
- History names must not contain `'/'`
- Must have at least one partial
- Compile-time function

**Description:**
Upon entry to a deep history pseudostate, the machine transitions to the deepest (most nested) leaf state that was previously active within the parent's subtree, or to a default target if no history exists.

### `dsl.Choice(name, partials...)`

Declares a choice pseudostate that evaluates a series of guarded transitions and takes the first one whose guard condition is satisfied.

**Parameters:**
- `name` — Choice pseudostate name. Cannot contain `'/'`.
- `partials...` — Two or more transitions; typically the last is a guardless fallback

**Constraints:**
- Choice names must not contain `'/'`
- Must have at least one transition
- Last transition should typically be guardless (fallback)
- Compile-time function

**Description:**
Implements conditional routing based on guard conditions. Each transition is evaluated in order, and the first with a successful guard is taken.

---

## Transitions

### `dsl.Transition(partials...)`

Declares a transition between states or pseudostates.

**Parameters:**
- `partials...` — Any combination of event trigger, target path, guard condition, effect action, and/or source state

**Constraints:**
- Compile-time function

**Description:**
Defines the structural elements of a state change: what event triggers it, where it goes, any conditions that must be met, actions to execute, and optionally the originating state.

### `dsl.Initial(partials...)`

Declares the initial transition when entering a composite state or the machine root.

**Parameters:**
- `partials...` — Target path and optional effect action

**Constraints:**
- Must include a target
- Compile-time function

**Description:**
Specifies the default entry point when a composite state (or the root machine) is entered. Typically paired with a target path and optional entry effect.

---

## Event Triggers

### `dsl.On(event_type)`

Declares a typed event trigger for a specific event type.

**Parameters:**
- `event_type` — Event type identifier (strongly-typed)

**Constraints:**
- Compile-time function

**Description:**
Specifies that a transition is triggered by a particular event type. The event system is statically type-checked at compile time.

### `dsl.On(event_name)` — String-based events

Declares a string-based event trigger using a string literal name.

**Parameters:**
- `event_name` — Event name (string literal). No character restrictions for the name itself.

**Constraints:**
- Compile-time function

**Description:**
Enables simple event triggering by string name, useful for events without structured data payloads.

### `dsl.OnCall(operation_name)` — Operation-triggered events

Declares a transition trigger linked to a named operation. Fires when that operation is invoked via `instance.Call<"operation_name">()`.

**Parameters:**
- `operation_name` — Operation name. Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
Routes transitions based on explicit operation invocation, allowing the state machine to respond to named procedure calls.

### `dsl.When(attribute_name)` / `dsl.OnSet(attribute_name)` — Attribute-change events

Declares an attribute-change trigger that fires when a named attribute is modified via `instance.Set<"attribute_name">(value)` or `instance.set("attribute_name", value)`. Attribute values can be read via `instance.Get<"attribute_name">()` or `instance.get("attribute_name")`.

**Parameters:**
- `attribute_name` — Attribute name. Cannot contain `'/'`.

**Constraints:**
- Attribute names must not contain `'/'`
- Compile-time function

**Description:**
Triggers a transition whenever a specific attribute value changes. Provides reactive attribute-based state management.

---

## Timing Events

### `dsl.After(duration_source)` — Timeout with duration source

Declares a timeout trigger that fires after a specified duration.

**Parameters:**
- `duration_source` — Either a callable returning a duration, or an attribute name (string literal) containing a duration value

**Constraints:**
- Compile-time function

**Description:**
Defines a time-based trigger. The duration can be statically specified via a callable, or dynamically specified via an attribute value. The timeout is relative (elapsed time) rather than absolute.

### `dsl.Every(interval_source)` — Periodic interval

Declares a periodic interval trigger that fires repeatedly at fixed intervals.

**Parameters:**
- `interval_source` — Either a callable returning a duration, or an attribute name (string literal) containing a duration value

**Constraints:**
- Compile-time function

**Description:**
Defines a repeating timer. The transition (typically a self-transition) fires on each interval expiration. Useful for polling, heartbeat, and periodic activity.

### `dsl.At(timepoint_source)` — Time-point trigger

Declares a time-point trigger that fires at a specific absolute time.

**Parameters:**
- `timepoint_source` — Either a callable returning a time-point, or an attribute name (string literal) containing a time-point value

**Constraints:**
- Compile-time function

**Description:**
Defines an absolute deadline trigger. Unlike `dsl.After()` (which is relative), this fires at a specific moment in time.

---

## Transition Targets & Routing

### `dsl.Target(path)`

Declares the target state of a transition using an absolute path.

**Parameters:**
- `path` — Absolute state path (string literal). Must start with `'/'`. Format: `/RootName/ParentName/ChildName`

**Constraints:**
- Path must be absolute (start with `'/'`)
- Compile-time function

**Description:**
Specifies the destination state for a transition using a hierarchical path notation. Paths are absolute from the root of the state machine.

### `dsl.Source(path)` — Explicit source routing

Specifies the source state of a transition for parent-level routing.

**Parameters:**
- `path` — Source state path (string literal)

**Constraints:**
- Compile-time function

**Description:**
Explicitly names the originating state of a transition. Enables transitions to be defined at a parent level while routing based on which child state the event originated from.

---

## State Behaviors

### `dsl.Entry(action...)`

Declares entry action(s) executed upon entering a state.

**Parameters:**
- `action...` — One or more action callables

**Constraints:**
- Compile-time function

**Description:**
Specifies behaviors that occur when the state is entered. Multiple entry actions execute in order.

### `dsl.Entry(operation_name...)` — Named operations

Declares entry actions as references to named model operations.

**Parameters:**
- `operation_name...` — One or more operation names (string literals). Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
References model-level operations to execute on state entry, enabling separation of model structure from implementation.

### `dsl.Exit(action...)`

Declares exit action(s) executed upon leaving a state.

**Parameters:**
- `action...` — One or more action callables

**Constraints:**
- Compile-time function

**Description:**
Specifies behaviors that occur when the state is exited. Multiple exit actions execute in order.

### `dsl.Exit(operation_name...)` — Named operations

Declares exit actions as references to named model operations.

**Parameters:**
- `operation_name...` — One or more operation names (string literals). Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
References model-level operations to execute on state exit.

### `dsl.Activity(action...)`

Declares state activity/ies — ongoing behaviors executed while in a state.

**Parameters:**
- `action...` — One or more activity callables

**Constraints:**
- Compile-time function

**Description:**
Specifies background or periodic behaviors that run while the state is active. Activities are distinct from entry/exit actions and represent ongoing work.

### `dsl.Activity(operation_name...)` — Named operations

Declares state activities as references to named model operations.

**Parameters:**
- `operation_name...` — One or more operation names (string literals). Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
References model-level operations to execute as state activities.

### `dsl.Effect(action...)`

Declares transition effect(s) — action(s) executed when traversing a transition.

**Parameters:**
- `action...` — One or more effect callables

**Constraints:**
- Compile-time function

**Description:**
Specifies behaviors executed during a transition, between the exit of the source state and the entry of the target state. Multiple effect actions execute in order.

### `dsl.Effect(operation_name...)` — Named operations

Declares transition effects as references to named model operations.

**Parameters:**
- `operation_name...` — One or more operation names (string literals). Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
References model-level operations to execute as transition effects.

---

## Guards & Deferral

### `dsl.Guard(condition)`

Declares a transition guard — a boolean condition that must be satisfied for the transition to execute.

**Parameters:**
- `condition` — A callable returning a boolean value

**Constraints:**
- Compile-time function

**Description:**
Specifies a condition that gates a transition. If the guard returns false, the transition does not occur and the event is typically discarded.

### `dsl.Guard(operation_name)` — Named operation

Declares a guard as a reference to a named model operation.

**Parameters:**
- `operation_name` — Operation name (string literal). Cannot contain `'/'`.

**Constraints:**
- Operation names must not contain `'/'`
- Compile-time function

**Description:**
References a model-level operation that implements the guard logic and returns a boolean.

### `dsl.Defer(event_type...)`

Declares that certain event types should be deferred (queued) while in a state, to be re-queued and processed upon exiting the state.

**Parameters:**
- `event_type...` — One or more event types to defer

**Constraints:**
- Compile-time function

**Description:**
Specifies event types that should not be processed in the current state but instead queued for later processing when the state is exited. Implements the deferral pattern for handling events at the appropriate state.

---

## Model Metadata

### `dsl.Attribute(name, type)`

Declares a model-level attribute of a specified type without a default value.

**Parameters:**
- `name` — Attribute name (string literal). Cannot contain `'/'`.
- `type` — Attribute type

**Constraints:**
- Attribute names must not contain `'/'`
- Compile-time function

**Description:**
Defines a named data member of the state machine with explicit type specification. At runtime, read via `instance.Get<"name">()` and write via `instance.Set<"name">(value)`. For dynamic/string-based access, use `instance.get("name")` and `instance.set("name", value)` (see [Runtime Attribute Access](#runtime-attribute-access)).

### `dsl.Attribute(name, type, default_value)` — With default

Declares a model-level attribute with a default/initial value.

**Parameters:**
- `name` — Attribute name (string literal). Cannot contain `'/'`.
- `type` — Attribute type (explicit)
- `default_value` — Initial value

**Constraints:**
- Attribute names must not contain `'/'`
- Compile-time function

**Description:**
Defines a named data member with an explicit type and initialization value.

### `dsl.Attribute(name, default_value)` — Type-deduced

Declares a model-level attribute with type deduced from the default value.

**Parameters:**
- `name` — Attribute name (string literal). Cannot contain `'/'`.
- `default_value` — Initial value (type is deduced)

**Constraints:**
- Attribute names must not contain `'/'`
- Compile-time function

**Description:**
Defines a named data member where the type is inferred from the provided default value.

### `dsl.Operation(name, implementation)`

Declares a named operation that can be invoked via explicit operation calls and that drives internal operation events.

**Parameters:**
- `name` — Operation identifier (string literal). Cannot contain `'/'`.
- `implementation` — Reference to the operation implementation (typically a member function pointer)

**Constraints:**
- Operation names must not contain `'/'`
- Implementation must be a callable reference (not a lambda or inline callable)
- Compile-time function

**Description:**
Registers a named operation in the model. Operations can be invoked via `instance.Call<"operation_name">()` and trigger corresponding transitions via `dsl.OnCall()` triggers. Enables clean separation between model structure and behavior implementation.

---

## Group Operations

### `dsl.MakeGroup(machines...)`

Factory function to create a group of multiple state machine instances.

**Parameters:**
- `machines...` — Two or more state machine instances

**Constraints:**
- Requires at least one machine
- Compile-time function

**Description:**
Combines multiple machines into a logical group for coordinated dispatch and management. All machines in the group can receive events simultaneously via `instance.Dispatch()` calls on the group.

### `dsl.MakeGroup(group_id, machines...)`

Factory function to create a group with an identifier.

**Parameters:**
- `group_id` — Group identifier (string)
- `machines...` — Two or more state machine instances

**Constraints:**
- Compile-time function

**Description:**
Creates an identified group of machines for tracking and coordinated operation.

---

## Type Utilities

### `dsl.MakeKind(id)` / `dsl.MakeKind(name)`

Constructs event kind identifiers with optional inheritance.

**Parameters:**
- `id` — Numeric event ID, OR
- `name` — Event name (string)
- `base_kinds...` — (Optional) Parent kinds for polymorphic inheritance

**Constraints:**
- Compile-time function

**Description:**
Creates event kind values that define the event type and optional inheritance hierarchy. Kinds are the underlying type system for events, enabling both strong typing and runtime polymorphism.

### `dsl.IsKind(kind, base_kind...)` — Kind matching

Checks if a kind matches or inherits from one or more base kinds.

**Parameters:**
- `kind` — Kind value to check
- `base_kind...` — One or more base kinds to match against

**Constraints:**
- Compile-time function

**Returns:** Boolean (true if `kind` inherits from any base)

**Description:**
Tests event kind membership, supporting polymorphic event matching. A kind inherits from a base kind if explicitly declared with that base.

---

## Runtime Attribute Access

Runtime string-based attribute accessors that complement the compile-time `Get<"name">()`/`Set<"name">(value)` methods. These use `std::any` for type erasure, enabling dynamic, language-agnostic access suitable for scripting bindings, serialization, and tooling.

### `instance.get(name)` — Runtime attribute read

Returns a type-erased copy of the named attribute's current value.

**Parameters:**
- `name` — Attribute name (`std::string_view`)

**Returns:** `std::any` — A copy of the attribute value, or an empty `std::any` if no attribute with the given name exists.

**Description:**
Looks up an attribute by name at runtime and returns its value wrapped in `std::any`. The caller extracts the value using `std::any_cast`. Returns an empty `std::any` (`.has_value() == false`) for unknown names.

**Limitations:**
- Returns a copy of the value (inherent to `std::any` type erasure)
- Caller must know the exact stored type for `std::any_cast` extraction

### `instance.set(name, value)` — Runtime attribute write

Updates a named attribute and emits any associated `dsl.When()` change events.

**Parameters:**
- `name` — Attribute name (`std::string_view`)
- `value` — New value (`const std::any&`) — must hold the exact attribute type

**Returns:** `result_t`
- `Processed` — Attribute was updated (or value was unchanged)
- `QueueFull` — Unknown attribute name or type mismatch

**Description:**
Looks up an attribute by name at runtime, extracts the value via `std::any_cast`, applies change detection, updates the storage, and emits a ChangeEvent if any `dsl.When()` listener is declared for that attribute. Mirrors the behavior of the compile-time `Set<"name">(value)` method.

**Limitations:**
- Requires exact type match via `std::any_cast` (no implicit conversions unlike the compile-time `Set`)
- Returns `QueueFull` for both unknown names and type mismatches (caller cannot distinguish the two)

---

## Runtime Constants

### Event Dispatch Results

**`QueueFull`** — Event could not be enqueued due to queue capacity

**`Processed`** — Event was successfully enqueued and will be processed

**`Deferred`** — Event was deferred for later processing per `dsl.Defer()` declaration

**Description:**
Return values from `instance.Dispatch(event)` indicating the outcome of an attempt to queue an event.

---