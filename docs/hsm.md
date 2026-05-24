# hsm: Compile-Time Hierarchical State Machine

`hsm` is a C++20 header-only library for hierarchical state machines (HSMs) where the full model is normalized at compile time. The goal is **predictable control flow**, **strong typing**, and **zero allocation in the model**.

## Overview

* **Header-only**: single `#include "hsm/hsm.hpp"`.
* **Declarative model**: `define(...)`, `state(...)`, `transition(...)`, `initial(...)`, etc.
* **Deep hierarchy**: nested states, composite states with `initial`, deep/shallow history, final states, choice states.
* **Thread-free cooperative scheduling**: timers and activities use C++20 coroutines with no `std::thread` dependency. The engine can be driven externally via `task.resume()`, making it suitable for bare-metal, FreeRTOS, and other embedded environments.
* **Time & async**: timers (`on_timeout`, `on_interval`, `on_timepoint`; UML aliases: `after`, `every`, `at`) and `activity(...)` for long-running work. Timers can be driven either by small functions or by attribute-backed configuration via string names.
* **Typed or named events**: `Event<kind>` types and string/named events (`on("fire")`, `dispatch<"fire">()`).
* **Unified, zero-allocation event queue**: a single, compile-time-sized ring buffer powers both deferral (`defer<T>()`) and events dispatched while the machine is already in transit; if your model never defers and never calls `dispatch` from behaviors, the queue effectively stays idle.

## Design principles

* **Decompose behavior into small states**
  * Prefer many small states with simple entry/exit/effect logic over huge behaviors that branch on internal flags.
  * Use composite states plus `initial(...)`, `final(...)`, `choice(...)`, and history (`deep_history`, `shallow_history`) to encode control flow explicitly.
  * Model long-lived phases as states, not booleans in your instance.

* **Treat the instance as data, only touched from behaviors**
  * `HSM<Model, Instance,...>` inherits from `Instance`, but **application code should not read or write instance fields directly on the state machine**.
  * Instead, expose data changes through `entry`, `exit`, `effect`, `guard`, and `activity` behaviors that receive `Instance&`.
  * Outside the machine, prefer to interact via events and (if really needed) read-only queries like `sm.state()`.
  * To **read values** out of the machine, use **query-style internal events** that carry a value or pointer/reference for the HSM to write into (e.g. `struct GetCount { int* out; };` with an `effect` that assigns `*evt.out = inst.counter;`).

* **Prefer named functions over inline lambdas in the model**
  * Keep models easy to scan by binding small, well-named functions: `entry(on_enter_idle)`, `effect(record_timeout)`, etc.
  * Implement behaviors as free functions or static member functions taking `(Signal&, Instance&, const EventBase&)` (or a concrete event type).
  * Lambdas are fine for tiny examples and tests, but in real models they tend to hide intent and make reuse harder.

* **Guards/effects/entry/exit must be side-effect free and non-blocking**
  * These behaviors run **synchronously inside a macrostep** (a single dispatch or completion step).
  * They may **read/write the `Instance`** but should not:
    * Perform I/O (logging, networking, disk), spawn threads, or acquire long-held locks.
    * Sleep or block on timers, futures, or condition variables.
  * Think of them as pure business rules plus cheap `Instance` mutations; all slow or externally-visible work belongs in `activity(...)` or code *outside* the state machine.

* **Use `activity(...)` for long-running or asynchronous work**
  * Attach `activity(fun)` to a state to run `fun(Signal&, Instance&, const EventBase&)` in the configured `Scheduler`.
  * Activities can:
    * Perform blocking I/O, long computations, or waits.
    * Observe `Signal` for cancellation (e.g., parent signal cancelled, timer expired).
  * Completion of an activity is combined with completion of child states:
    * A composite state only finishes when **its final substate is reached and its activity completes**, then completion transitions fire.

* **Exploit hierarchy instead of flattening**
  * Model parent phases (`Working`, `Idle`, `Interrupted`, `Finished`) as parents with nested substates for detailed steps.
  * Use **history** (`deep_history`, `shallow_history`) to return to prior nested configurations after interrupts instead of manually tracking “where we were”.
  * Let parent states own cross-cutting behavior (shared guards/effects/deferral) that children inherit structurally.

## Minimal example

```cpp
#include "hsm/hsm.hpp"
#include <cassert>

using namespace hsm;

// Forward declaration for CRTP
struct Machine;

// Side-effect-free, non-blocking behavior: updates only the Machine instance.
void bump_on_entry(Signal&, Machine& m, const EventBase&) {
    m.counter++;
}

constexpr auto model = define("Machine",
    initial(target("StateA")),
    state("StateA",
        entry(bump_on_entry),
        transition(on("next"), target("StateB"))
    ),
    state("StateB",
        transition(on("reset"), target("StateA"))
    )
);

// Define the machine using CRTP (Curiously Recurring Template Pattern)
struct Machine : HSM<model, Machine> {
    int counter = 0;
};

int main() {
    Machine sm;
    sm.start();       // Initialize and enter /Machine/StateA

    assert(sm.state() == "/Machine/StateA");
    assert(sm.counter == 1);             // entry ran once

    sm.dispatch<"next">();               // Literal-named event
    assert(sm.state() == "/Machine/StateB");

    sm.dispatch<"reset">();
    assert(sm.state() == "/Machine/StateA");
}
```

## Attributes: model-level configuration and change events

Attributes are **named, typed pieces of configuration/state** declared on the model rather than as raw fields on your instance type. They enable:

* Centralized documentation of configuration
* Compile-time name-based access (`get<"name">`, `set<"name">`)
* Declarative **change-triggered transitions** via `on_set("name")` (alias: `when("name")`)

### Declaring attributes

Use `attribute<T>("name", default)` at the top-level inside `define(...)`:

```cpp
constexpr auto attr_model = define(
    "attr_machine",
    attribute<int>("value", 0),
    attribute("flag", false),        // type-deducing overload (T = bool)
    initial(target("/attr_machine/idle")),
    state("idle"));
```

Key points:

* Attribute names are compile-time fixed strings; they cannot contain `/`.
* You can either specify the type explicitly, or let it be deduced from the default value.

### Accessing attributes at runtime

Given `struct Machine : HSM<attr_model, Machine> {};`:

```cpp
Machine sm;
sm.start();

// Read/write by name at compile time
int  v = sm.get<"value">();
bool f = sm.get<"flag">();

sm.set<"value">(42);
sm.set<"flag">(true);
```

`get<"name">()` and `set<"name">(value)` are **fully type-checked** at compile time:

* If the attribute is not declared, you get a static\_assert.
* If the value is not convertible to the declared type, compilation fails.

### Attribute-driven transitions: `on_set` / `when`

You can attach transitions that fire **whenever the attribute value changes** via `on_set("name")` (the recommended `on_*`-style spelling):

```cpp
struct Machine;

void on_value_changed(Signal&, Machine& m, const AnyEvent&) {
  m.on_value_changed++;
}

constexpr auto when_model = define(
    "when_machine",
    attribute("value", 0),
    initial(target("/when_machine/idle")),
    state("idle",
          transition(on_set("value"),
                     target("/when_machine/idle"),
                     effect(on_value_changed))));
```

The change semantics are:

* `set<"value">(x)` compares the new value against the old value.
* If it is **different**, a synthetic `ChangeEvent` is dispatched with kind derived from the attribute name.
* Any transitions with `on_set("value")` or `when("value")` will see that event.
* If the value is unchanged, no event is emitted.

For users who prefer UML terminology, `when` is just an alias:

* `when("name")` ≡ `on_set("name")`

```cpp
transition(on_set("value"), target("/machine/ready"), effect(...));
```

### Constructor-time overrides: `set` / `emplace`

Sometimes you want to override attributes **when constructing** an HSM instance, without firing any change events. Use `hsm::set<"name">(...)` and `hsm::emplace<"name">(...)` with the dedicated HSM constructors:

```cpp
struct Machine : AttrInstance, HSM<when_model, Machine> {
  using Base = HSM<when_model, Machine>;
  using Base::Base; // inherit HSM constructors
};

Machine sm_default; // uses model defaults

Machine sm_overrides(
    hsm::set<"value">(42),
    hsm::set<"flag">(true));

Machine sm_emplaced(
    hsm::emplace<"name">("hello"),
    hsm::emplace<"vec">(3, 7));
```

These constructor-time overrides:

* Write directly into attribute storage
* **Do not** emit ChangeEvent-kind events
* Therefore **do not** drive `when("name")`/`on_set("name")` transitions during construction

This pattern is ideal for configuration-style attributes that should be initialized once and then reacted to dynamically via `set<"name">(...)`.

## Operations: typed, named calls into the machine

Operations provide a **compile-time API** for calling named functions on your HSM instance via `sm.call<"name">(args...)` while still routing a corresponding event through the state machine first.

### Declaring operations in the model

Use `operation("name", callable)` at the model level:

```cpp
struct Machine;

constexpr auto op_model = define(
    "op_machine",
    operation("do_something", &Machine::do_something_impl),
    initial(target("/op_machine/idle")),
    state("idle"));

struct Machine : HSM<op_model, Machine> {
  int counter = 0;

  void do_something_impl(int x) {
    counter += x;
  }
};
```

`operation("name", callable)` supports:

* Pointer-to-member functions: `&Machine::fn(Args...)`
* Free/static function pointers: `&fn(Args...)`

At normalization time, each operation also contributes a **`CallEvent`-kind entry** to the model’s event table keyed by the operation name.

### Invoking operations: `HSM::call` and `CallEvent`

To call an operation, use the `call<"name">(args...)` method on your HSM instance:

```cpp
Machine sm;
sm.start();

sm.call<"do_something">(5);   // counter += 5
sm.call<"do_something">(7);   // counter += 7
```

`HSM::call<Name>(Args&&...)` guarantees at compile time that:

* An operation with that name exists in the model
* The argument count matches the underlying callable signature
* The argument types are convertible to the callable’s parameter types

Under the hood, `call<"do_something">(args...)`:

1. Constructs a `CallEvent<Name, std::tuple<Args...>>` carrying the arguments
2. Computes the associated `CallEvent` kind from the name
3. Dispatches that event through the normal transition/behavior pipeline
4. Only then invokes the underlying callable via `std::invoke`

This allows you to:

* Attach guards/effects/entry/exit that react to **operation invocations**
* Keep your operation bodies simple, while still letting the HSM observe and orchestrate them

### Using operations as behaviors by name

In addition to `HSM::call`, you can reference operations directly from behaviors by **operation name**, without emitting `CallEvent`s:

```cpp
struct MachineBase {
    int counter = 0;
    void bump() { ++counter; }
};

constexpr auto model = define(
    "by_name",
    operation("bump", &MachineBase::bump),
    initial(target("/by_name/idle")),
    state("idle",
          // entry runs the "bump" operation body directly
          entry("bump")));
```

Name-based behaviors support:

* `entry("op")`, `exit("op")`, `effect("op1", "op2", ...)`, `activity("op")`
* `guard("op")`

These forms are resolved **at compile time** against `operation("name", callable)` declarations in the same model:

* If the operation name does not exist, compilation fails with a clear diagnostic.
* The underlying callable must be invocable by the normal behavior matrix (e.g. `void f(Signal&, Self&, const EventBase&)`, `void f(Self&)`, `bool f(Self&)`, etc.).
* For guards, the result is converted to `bool`; `detail::not_invoked` is treated as `false`.

Importantly, name-based behaviors **do not emit `CallEvent`s** and do **not** go through `HSM::call` – they simply run the bound operation body as an entry/exit/effect/activity/guard.

### Reacting to operations: `on_call` and `CallEvent`

To declare transitions that fire when an operation is called, use `on_call("name")`:

```cpp
struct OnCallMachine;

constexpr auto on_call_model = define(
    "on_call_machine",
    operation("do_something", &OnCallMachine::do_impl),
    initial(target("/on_call_machine/idle")),
    state("idle",
          transition(
              on_call("do_something"),
              effect(&OnCallMachine::on_do_something),
              target("/on_call_machine/idle"))));

struct OnCallMachine : HSM<on_call_model, OnCallMachine> {
  std::vector<std::string> log;
  int last_body_arg{0};

  void do_impl(int value) {
    log.push_back("body");
    last_body_arg = value;
  }

  static void on_do_something(OnCallMachine &self) {
    self.log.push_back("effect");
  }
};

// Usage
OnCallMachine sm;
sm.start();
sm.call<"do_something">(7);
// log == {"effect", "body"}; last_body_arg == 7
```

`on_call("name")` is sugar for “listen to the `CallEvent` associated with the named operation”:

* It creates a trigger whose event kind is `make_kind(fnv1a_64("name"), Kind::CallEvent)`
* `HSM::call<"name">(...)` dispatches a `CallEvent` with the same kind
* Any transitions with `on_call("name")` will see that event and fire before the operation body runs

For more advanced use cases, you can reference the `CallEvent` type directly:

```cpp
using DoEvent = hsm::CallEvent<
    hsm::detail::make_fixed_string("do_something"),
    std::tuple<int>>;

void on_do_event(Signal&, Machine& m, const DoEvent& e) {
    auto arg = std::get<0>(e.args);
    // inspect arg or record metrics
}

transition(on<DoEvent>(), effect(on_do_event),
          target("/machine/idle"));
```

### Capability queries

You can ask at compile time whether a machine supports a given operation or call-event type:

```cpp path=null start=null
static_assert(Machine::template supports_operation<"do_something">());
static_assert(Machine::template supports_event<DoEvent>());
```

You can also derive the canonical event type associated with a name using `Machine::template events<Name>` and then query support:

```cpp path=null start=null
using OpEvt   = typename Machine::template events<"do_something">::type;  // CallEvent-kind
using AttrEvt = typename Machine::template events<"value">::type;         // ChangeEvent-kind
using NameEvt = typename Machine::template events<"power_on">::type;      // regular Event kind

static_assert(Machine::template events<"do_something">::is_operation);
static_assert(Machine::template events<"value">::is_attribute);
static_assert(Machine::template events<"power_on">::is_plain_event);

static_assert(Machine::template events<"do_something">::supported());
```

`events<Name>` resolves operation names to `CallEvent` types, attribute names to `ChangeEvent`-kind events, and all other names to regular `Kind::Event` named events; the nested `supported()` is a shorthand for `Machine::template supports_event<type>()`.

This is particularly useful for generic utilities that must adapt to different models without instantiating unsupported operations.

## DSL overview and non-UML aliases

The DSL is intentionally small and regular. Here is the core surface, including the **aliases** for users who don’t know UML terminology.

### Structure

* `define("Machine", ...)` — root model definition
* `state("Name", ...)` — state definition
* `initial(transition(...))` or `initial(target("/..."))` — initial entry
* `final("Name")` — final state
* `choice("Name", ...)` — choice pseudostate
* `target("/Machine/parent/child")` — transition target
* `shallow_history("/Parent")`, `deep_history("/Parent")` — history pseudostates

### Events and transitions

* `transition(...)` — transition container
* `on<EventType>()` — typed event trigger
* `on("name")` — string/named event trigger
* `on_call("name")` — trigger for a named operation’s `CallEvent`

In day-to-day code, prefer the `on*` spellings (`on("name")`, `on<Event>()`, `on_set`, `on_timeout`, etc.); the UML-style names (`after`, `every`, `at`, `when`) are provided as aliases for people who like that vocabulary, but you do not need UML knowledge to use the library.

### Behaviors

* `entry(f...)`, `exit(f...)` — entry/exit behaviors for a state
* `effect(f...)` — transition effects
* `activity(f...)` — long-running work associated with a state
* `guard(f)` — transition guard

For readability you should usually bind small named functions or static member functions rather than large inline lambdas.

If you are already exposing operations via `operation("name", callable)`, you can also bind them by **name**:

* `entry("op")`, `exit("op")`, `effect("op1", "op2", ...)`, `activity("op")`
* `guard("op")`

These resolve at compile time to the corresponding operations and invoke their callables directly, without going through `HSM::call` or generating `CallEvent`s.

### Timers

Timers integrate into the model as normal `on_*`-style triggers and now support **two forms**:

* A **callable** that returns a duration or time point.
* A **string attribute name**, where the attribute value supplies the duration or time point.

APIs:

* `on_timeout(source)` — one-shot delay where `source` is either:
  * `Duration fn(Signal&, Machine&, const EventBase&)`, or
  * a string literal naming an attribute whose type is a `std::chrono::duration`.
* `on_interval(source)` — periodic timer; same `source` options as `on_timeout`.
* `on_timepoint(source)` — fire at a specific time point; `source` is either:
  * `TimePoint fn(Signal&, Machine&, const EventBase&)`, or
  * a string literal naming an attribute whose type is a `std::chrono::time_point`.

When you use an attribute name as the `source`, its type is validated at
compile time:

* For `on_timeout` / `after` and `on_interval` / `every`, the attribute type must
  be a `std::chrono::duration`.
* For `on_timepoint` / `at`, the attribute type must be a `std::chrono::time_point`
  whose `clock` matches the `Clock` template parameter of the `HSM`.

For periodic timers (`on_interval` / `every`), the attribute value is read each
time the timer loop re-arms, so changing the attribute at runtime changes the
period of subsequent ticks. For one-shot timers (`on_timeout` / `after` and
`on_timepoint` / `at`), the attribute is read each time the timer is armed (for
example when entering the state that owns the timer).

For users who prefer UML naming, the following aliases are provided and are exact equivalents for **both** forms:

* `after(source)` ≡ `on_timeout(source)`
* `every(source)` ≡ `on_interval(source)`
* `at(source)` ≡ `on_timepoint(source)`

Examples:

```cpp
using namespace std::chrono_literals;

// 1) Function-driven timeout
std::chrono::milliseconds wait_half_second(Signal&, Machine&, const EventBase&) {
    return 500ms;   // after 500ms, dispatch timer event
}

// 2) Attribute-driven timeout
constexpr auto model = define(
    "timer_machine",
    attribute<std::chrono::milliseconds>("timeout", 500ms),
    initial(target("/timer_machine/waiting")),
    state("waiting",
          // Uses the current value of attribute "timeout" for the delay
          transition(on_timeout("timeout"),
                     target("/timer_machine/expired"))));
```

### Attribute change

* `attribute<T>("name", default)` — declare an attribute; timer-friendly attributes typically use `std::chrono::duration` or `std::chrono::time_point` types.
* `on_set("name")` — `on_*`-style trigger on `set<"name">(...)` changes
* `when("name")` — UML-style alias for `on_set("name")`

Example:

```cpp
constexpr auto attr_model = define(
    "attr_machine",
    attribute("value", 0),
    initial(target("/attr_machine/idle")),
    state("idle",
          transition(on_set("value"),
                     target("/attr_machine/idle"),
                     effect(on_value_changed))));
```

Here `on_value_changed` will run every time `sm.set<"value">(...)` changes the value.

## Runtime surface (for humans and AI tools)

* **Main type**

```cpp
template <auto Model,
          typename Self,      // The CRTP derived type
          typename ClockT = hsm::Clock,
          typename QueuePolicy = hsm::DefaultQueuePolicy<Model>,
          typename TaskT = hsm::Task<ClockT>,
          typename AwaitableT = hsm::Awaitable<ClockT>,
          typename SignalT = hsm::Signal,
          std::size_t FrameSize = 256,
          template<std::size_t, std::size_t> typename PromisePoolT = hsm::PromisePool>
struct HSM;
```

* **Coroutine Frame Pool**

  HSM uses a fixed-size pool for coroutine frame allocation, eliminating heap allocation at runtime. The pool size is computed at compile time from the model's timer and activity count.

  * `FrameSize`: Maximum size in bytes for a single coroutine frame (default: 256). Increase if you get assertion failures about frame size.
  * `PromisePoolT`: Template template parameter for the pool allocator. The default `hsm::PromisePool` uses an O(1) free list with fixed storage—no heap allocation.

  Pool initialization is deferred to `start()` to avoid static initialization issues (important for embedded systems like FreeRTOS).

* **Construction**
  * **CRTP Pattern**: The `HSM` type is designed to be inherited from.
    ```cpp
    struct MyMachine : hsm::HSM<model, MyMachine> {
        // Your data members here
        int x = 0;

        // Expose constructors
        using HSM::HSM;
    };
    ```
  * **Arguments**:
    * `HSM()`: Default constructor.
    * `HSM(id)`: Initialize with a string ID.
    * `HSM(signal)`: Initialize with a parent signal (for hierarchy/cancellation).
    * `HSM(id, signal)`: ID and parent signal.

* **Start**
  * `TaskType start()`: Initializes the HSM (enters initial state) and returns a `Task` representing the engine coroutine.
  * `TaskType start(SignalType& signal)`: Same as above, with an external cancellation signal.
  * The returned `Task` can be:
    * **Fire-and-forget**: `auto task = sm.start();` — HSM is ready for dispatch immediately.
    * **Driven externally**: Call `task.resume()` to advance timers and process events.
    * **Blocking await**: `sm.start().await();` — blocks until the engine terminates (not typical for most use cases).
    * **Coroutine await**: `co_await sm.start();` — yields until the engine terminates.

* **Dispatch**
  * `void dispatch<EventType>()` or `void dispatch(const T& e)`: Typed event dispatch.
  * `void dispatch<"name">()`: Named event dispatch.
  * Normal dispatch and attribute set have no public result code; drive the task to observe completion.
  * All dispatch calls enqueue to the lock-free queue and wake the engine—safe from any context including ISRs.
  * While a dispatch is in progress (a *macrostep*), any nested calls to `dispatch`, `set<"name">`, `call<"name">`, or timer callbacks enqueue events into the unified queue; they are processed after the current macrostep completes.

* **Queries**
  * `std::string_view state() const;` — fully-qualified active leaf path, e.g. `"/Machine/Composite/SubState"`.
  * `std::string_view id() const;` — the machine's ID (if provided at construction).
  * `bool started() const;` — returns `true` if the HSM has been started.
  * `instance()` — internal accessor for behaviors to get the `Self&`.

### Dispatch Patterns

**Fire-and-forget (non-timer machines):**
```cpp
Machine sm;
sm.start();                // Initialize
sm.dispatch<"event">();    // Enqueue and process immediately
```

**Explicit engine driving (timer-based or batched):**
```cpp
Machine sm;
auto task = sm.start();    // Get engine task

sm.dispatch<Event1>();     // Enqueue
sm.dispatch<Event2>();     // Enqueue
task.resume();             // Process all queued events

// Drive until complete:
while (!task.done()) {
    if (auto dl = task.deadline()) {
        // Wait until deadline or external wake
    }
    task.resume();
}
```

**ISR-safe dispatch:**
```cpp
// Safe from interrupt handlers - same API, no special handling
void ISR_Handler() {
    g_machine.dispatch(ButtonPressed{});  // Enqueues + wakes engine
}
```

## Multi-Machine Dispatch (`hsm::Group`)

`hsm::Group` enables managing multiple state machines as a single unit. It provides optimized dispatching strategies.

```cpp
#include "hsm/group.hpp"

struct M1 : hsm::HSM<m1_model, M1> { using HSM::HSM; };
struct M2 : hsm::HSM<m2_model, M2> { using HSM::HSM; };

M1 m1("service_A");
M2 m2("service_B");

// Start both machines
auto group = hsm::make_group(m1, m2);
auto driver = group.start();  // Starts all machines and returns a GroupTask driver
```

### Driving Groups (`hsm::GroupTask`)

`group.start()` returns a **GroupTask** that mirrors `Task` but iterates over all machines in the group. Use it to drive all engines from a single loop:

```cpp
group.dispatch(StartEvent{});
driver.resume();  // Drives every machine in the group once

// Timer-based groups: drive until done (or forever for non-terminating engines)
while (!driver.done()) {
    if (auto dl = driver.deadline()) {
        // Wait until earliest deadline or external wake
    }
    driver.resume();
}
```

### 1. Flattened Broadcast (`group.dispatch(event)`)

When you call `group.dispatch(event)` (without an ID), the event is broadcast to **all** machines in the group.

* **Optimization**: The group flattens the hierarchy at compile-time. If you have nested groups (e.g., a Group containing another Group), `hsm` recursively expands them into a single flat tuple of machine references.
* **Performance**: Iteration is linear O(N) over the total number of leaf machines, with no recursive function call overhead during dispatch.
* **Completion**: Dispatch returns no result. Drive each machine's task to observe processing completion.

### 2. Targeted Dispatch (`group.dispatch("id", event)`)

When you call `group.dispatch("service_A", event)`, the group routes the event to a specific machine.

* **Recursive Lookup**: The lookup checks:
  1. Does a direct child match the ID?
  2. If a child is a `Group`, recursively delegate the ID search to it.
* **Performance**: This is a runtime search (linear/recursive over the structure). For static sets of machines, this is efficient enough for typical command routing.
* **Result**: Targeted dispatch returns no result. Unknown targets are ignored.

### Unified Interface

`Group` exposes the same `dispatch` signature as `HSM` (except for the ID-based overload), allowing groups to be nested or treated interchangeably with individual machines in generic code.

## Unified event queue and QueuePolicy

The last template parameter of `HSM` is a `QueuePolicy` that controls the capacity
of the **unified internal event queue**. This queue is used both for
`defer<T>()` and for events dispatched while the machine is already in
transit (run-to-completion semantics).

* **`hsm::DefaultQueuePolicy<Model>`**: The default; sets
  `QueuePolicy::capacity` to the model’s `normalized_model.transition_count`
  (clamped to at least 1). This is a good general-purpose setting.
* **Custom policies**: Any type with
  `static constexpr std::size_t capacity;`. Use this when you need to
  shrink memory footprint or allow more in-flight events.

On overflow (when the queue is full), `dispatch(...)` returns `false` and the
new event is not accepted.

Usage:

```cpp
struct Queue32Policy {
  static constexpr std::size_t capacity = 32;
};

using MyMachine = hsm::HSM<model, MyMachine,
                             hsm::Clock,
                             Queue32Policy>;
```

## Cooperative Scheduling (Thread-Free)

hsm uses C++20 coroutines for **thread-free cooperative scheduling**. Timers and activities do not spawn threads; instead, they suspend as coroutines and are resumed by an external driver (your main loop, RTOS task, or test harness).

### Core Types

* **`hsm::Task<ClockType>`**: A coroutine wrapper that tracks deadlines and supports:
  * `bool done()` — check if the coroutine has completed.
  * `bool resume()` — advance the coroutine; returns `true` if more work remains.
  * `void await()` — blocking wait until done (calls `resume()` in a loop).
  * `std::optional<time_point> deadline()` — the deadline this task is waiting for (if any).
  * Awaitable interface — can be `co_await`ed from other coroutines.

* **`hsm::Awaitable<ClockType>`**: A cooperative awaitable used internally by timers. Suspends until a deadline passes or a signal is set. Stores the deadline in the coroutine's promise for the engine to query.

* **`hsm::Signal`**: A cancellation signal with parent chaining. Used to cancel timers/activities when exiting states.

### Engine Model

When you call `sm.start()`, the HSM:
1. Enters the initial state (runs entry behaviors, follows initial transitions).
2. Returns a `Task` containing the **engine loop**.

The engine loop is a cooperative scheduler that:
1. Suspends on `co_await Awaitable{next_deadline, &wake_signal}`.
2. Resumes when the deadline passes or `wake_signal` is set (e.g., by `dispatch()`).
3. Resumes timer coroutines whose deadlines have passed.
4. Processes queued events.
5. Repeats until terminated.

### Driving the Engine

For **non-timer use cases**, simply call `start()` and the HSM is ready:

```cpp
Machine sm;
sm.start();  // HSM is initialized
sm.dispatch<"event">();  // Dispatches work immediately
```

For **timer-based use cases**, store the task and call `resume()` to advance time:

```cpp
Machine sm;
auto engine = sm.start();

// Advance simulated time (or wait for real time to pass)
SimulatedClock::advance(100ms);
engine.resume();  // Engine checks timers, fires any that are ready

// Or integrate with your main loop:
while (!engine.done()) {
    auto deadline = engine.deadline();
    if (deadline) {
        sleep_until(*deadline);  // Platform-specific sleep
    }
    engine.resume();
}
```

### Custom Clock Types

The `ClockType` template parameter (default: `std::chrono::steady_clock`) can be customized for testing or embedded systems:

```cpp
// Simulated clock for deterministic testing
struct SimulatedClock {
    using duration = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<SimulatedClock>;
    static constexpr bool is_steady = true;

    static std::atomic<int64_t> current_time_ms;

    static time_point now() noexcept {
        return time_point(duration(current_time_ms.load()));
    }

    static void advance(duration d) {
        current_time_ms.fetch_add(d.count());
    }
};

// Use with HSM
struct Machine : HSM<model, Machine, SimulatedClock> {};

Machine sm;
auto engine = sm.start();

SimulatedClock::advance(500ms);
engine.resume();  // Timers see 500ms have passed
```

### Benefits

* **No `std::thread` dependency**: Works on bare-metal, FreeRTOS, or any environment.
* **Deterministic testing**: Use a simulated clock and explicit `resume()` calls.
* **No polling/busy-wait**: The engine suspends until work is ready.
* **Composable**: Multiple HSMs can share an event loop or be driven independently.

## Performance

hsm achieves high throughput through compile-time dispatch optimization while maintaining safety guarantees required for embedded and ISR-safe operation.

### Benchmark Results (Apple M1 Pro, Release build)

| Scenario | Dispatches/sec | vs QP-Framework |
|----------|----------------|-----------------|
| Simple ping-pong (A↔B) | ~123M | +65% |
| Hierarchical (parent/child) | ~69M | -7% |
| Deep hierarchy (3 levels) | ~67M | -6% |
| Guarded transitions | ~127M | -4% |
| Complex (traffic light) | ~69M | +5% |

### Performance Characteristics

**ISR-safe dispatch**: All `dispatch()` calls unconditionally enqueue events and wake the engine. This guarantees safe dispatch from interrupt handlers without special handling - the same code path works from ISR and non-ISR contexts.

**Lock-free queue**: The unified event queue uses atomic operations with `memory_order_seq_cst` for MISRA compliance and ISR safety. Events dispatched during a macrostep (nested dispatch, timer callbacks, attribute changes) are queued and processed after the current transition completes.

**Zero heap allocation**: All coroutine frames use a fixed-size pool allocated within the HSM instance. Pool size is computed at compile time from the model's timer and activity count.

### Comparison Context

hsm is designed for **safety and predictability** rather than raw speed:
- Thread-free cooperative scheduling (no `std::thread` dependency)
- ISR-safe dispatch with proper atomics
- Compile-time model validation
- Deep hierarchy support with proper LCA computation

For comparison, a hand-written switch statement achieves ~7B dispatches/sec but lacks hierarchy, validation, and safety guarantees. hsm provides these features while maintaining competitive performance with other production HSM frameworks.

## Best Practices for High-Performance State Machines

### 1. Decompose into small states
- Prefer many small states over large states with internal flags
- Use hierarchy (`state("parent", state("child", ...))`) to organize phases
- Let state transitions express control flow, not conditionals in behaviors

### 2. Keep behaviors fast
- `entry`, `exit`, `effect`, `guard` run synchronously in the macrostep
- No I/O, no blocking, no long computations
- Move slow work to `activity(...)` which runs in the scheduler

### 3. Use the queue correctly
- `dispatch()` always enqueues—it never processes inline
- Multiple dispatches batch naturally; one `task.resume()` drains all
- Queue pressure is not surfaced as a dispatch result

### 4. ISR safety
- All dispatch methods are ISR-safe by design
- Uses `memory_order_seq_cst` atomics for MISRA compliance
- No special ISR wrappers needed—same API everywhere

### 5. Minimize allocations
- Queue capacity is compile-time sized via `QueuePolicy`
- Coroutine frames use a fixed pool (no heap)
- Attributes stored inline in HSM instance

## When to use which construct

* **Use `state(...)` + `initial(...)`** to build nested composites instead of flags.
* **Use `final("Name")` and completion transitions** to encode “done” conditions declaratively.
* **Use `choice("Name", ...)` with a guardless fallback** when branching on instance data.
* **Use `defer<Event...>()`** when events should be queued and replayed in a later state.
* **Use timers**:
  * `on_timeout(source)` (alias: `after(source)`) for one-shot delays, where `source` is either a function or an attribute name.
  * `on_interval(source)` (alias: `every(source)`) for periodic ticks, where `source` is either a function or an attribute name.
  * `on_timepoint(source)` (alias: `at(source)`) for absolute time, where `source` is either a function or an attribute name.
* **Use attribute change events**:
  * Declare attributes with `attribute<T>("name", default)` at the model level.
  * Use `on_set("name")` (alias: `when("name")`) to react declaratively when that attribute changes via `hsm.set<"name">(value)`.
* **Use `activity(...)` plus hierarchy** for any long-running or cancellable work; keep guards/effects/entry/exit simple and fast.

This style keeps the model structurally explicit, makes behavior easy to reason about, and keeps both human readers and AI tools aligned on how to extend the machine safely.

## Kitchen-sink example: thermostat

This example pulls the pieces together into a small but realistic thermostat. It demonstrates:

* Attributes for configuration (`target_c`, `hysteresis_c`, `sample_period`, `boost_timeout`)
* Attribute-driven transitions via `on_set("target_c")`
* Typed events (`SensorSample`, `FaultDetected`)
* Named events (e.g., `"power_on"`)
* Operations callable via `call<"...">` plus `on_call("...")`
* Operations reused directly as behaviors by name: `entry("...")`, `exit("...")`, `effect("...")`, `guard("...")`, `activity("...")`
* Composite states, `choice(...)`, and deep history
* Timers driven by attribute-backed durations (see the timer section above for function-based sources)
* Deferral of events while in a certain state
* Activities for long-running work

```cpp
#include "hsm/hsm.hpp"
#include <chrono>
#include <string>
#include <vector>

using namespace hsm;
using namespace std::chrono_literals;

// Forward declaration for CRTP and operations
struct Thermostat;

// Typed, named events (derive from hsm::named_event<"name">)
struct SensorSample
    : hsm::named_event<"sensor.sample"> {
    double temp_c{};
};

struct FaultDetected
    : hsm::named_event<"fault.detected"> {
    std::string message;
};

// Behavior declarations (bodies omitted for brevity)
void on_enter_off(Signal&, Thermostat&, const EventBase&);
void on_enter_idle(Signal&, Thermostat&, const EventBase&);
void on_enter_heating(Signal&, Thermostat&, const EventBase&);
void on_enter_cooling(Signal&, Thermostat&, const EventBase&);

void on_sample(Signal&, Thermostat&, const SensorSample&);
void on_target_changed(Signal&, Thermostat&, const AnyEvent&);
void on_set_target_called(Signal&, Thermostat&, const EventBase&);

bool guard_should_heat(const Thermostat&);
bool guard_should_cool(const Thermostat&);

void run_heating_activity(Signal&, Thermostat&, const EventBase&);
void run_cooling_activity(Signal&, Thermostat&, const EventBase&);

// Full thermostat model
constexpr auto thermostat_model = define(
    "thermostat",

    // Attributes (can be overridden at construction time)
    attribute<double>("target_c", 21.0),
    attribute<double>("hysteresis_c", 0.5),
    attribute<std::chrono::milliseconds>("sample_period", 500ms),
    attribute<std::chrono::milliseconds>("boost_timeout", 10min),

    // Operations (exposed as sm.call<"...">(...))
    operation("set_target", &Thermostat::set_target_impl),
    operation("boost", &Thermostat::boost_impl),

    // Operations reused directly as behaviors by name
    // (entry/exit/effect/activity/guard("..."))
    operation("enter_off", on_enter_off),
    operation("enter_idle", on_enter_idle),
    operation("enter_heating", on_enter_heating),
    operation("enter_cooling", on_enter_cooling),
    operation("handle_sample", on_sample),
    operation("handle_target_change", on_target_changed),
    operation("log_set_target", on_set_target_called),
    operation("heating_activity", run_heating_activity),
    operation("cooling_activity", run_cooling_activity),
    operation("should_heat", guard_should_heat),
    operation("should_cool", guard_should_cool),

    // Initial configuration: enter Running/Idle
    initial(target("/thermostat/Running/Idle")),

    // Keep track of the last Running substate when we leave it
    deep_history("/thermostat/Running"),

    // OFF: ignore most work, but remember incoming sensor samples
    state("Off",
          entry("enter_off"),
          defer<SensorSample>(),
          transition(on("power_on"),
                     target("/thermostat/Running/history"))),

    // FAULT: entered when a FaultDetected event arrives from anywhere
    state("Fault",
          transition(on<FaultDetected>(),
                     target("/thermostat/Fault")),
          transition(on("reset"),
                     target("/thermostat/Running/history"))),

    // RUNNING: main behavior, with nested Idle/Heating/Cooling
    state("Running",
          // React whenever the target temperature attribute changes
          transition(on_set("target_c"),
                     target("/thermostat/Running/Decide"),
                     effect("handle_target_change")),

          // Periodic sampling driven by an attribute-backed interval
          transition(on_interval("sample_period"),
                     target("/thermostat/Running/Idle")),

          // Observe operation calls
          transition(on_call("set_target"),
                     effect("log_set_target"),
                     target("/thermostat/Running/Decide")),

          // Nested substates
          state("Idle",
                entry("enter_idle"),
                transition(on<SensorSample>(),
                           effect("handle_sample"),
                           target("/thermostat/Running/Decide"))),

          choice("Decide",
                 transition(guard("should_heat"),
                            target("/thermostat/Running/Heating")),
                 transition(guard("should_cool"),
                            target("/thermostat/Running/Cooling")),
                 // Fallback: stay idle
                 transition(target("/thermostat/Running/Idle"))),

          state("Heating",
                entry("enter_heating"),
                activity("heating_activity"), // long-running I/O or blocking work
                transition(on<SensorSample>(),
                           effect("handle_sample"),
                           target("/thermostat/Running/Decide")),
                // Optional "boost" timeout driven by an attribute
                transition(on_timeout("boost_timeout"),
                           target("/thermostat/Running/Idle"))),

          state("Cooling",
                entry("enter_cooling"),
                activity("cooling_activity"),
                transition(on<SensorSample>(),
                           effect("handle_sample"),
                           target("/thermostat/Running/Decide"))));

// Machine instance
struct Thermostat : HSM<thermostat_model, Thermostat> {
    using HSM::HSM; // inherit HSM constructors

    // Internal data
    double last_measured_c{0.0};
    std::vector<std::string> log;

    // Operation bodies (called via sm.call<"...">)
    void set_target_impl(double new_target_c);
    void boost_impl();

    // Helpers used from behaviors
    void append_log(const std::string& line);
};
```
