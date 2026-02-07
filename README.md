# HSM: Compile-Time Hierarchical State Machine for C++20

A modern, header-only C++20 library for building compile-time hierarchical state machines with maximum performance and build-time validation.

## Features

* **Hierarchical States**: Support for nested states with proper entry/exit cascading.
* **Clean DSL**: Declarative syntax for defining states, transitions, and events.
* **Compile-Time Validation**: Catches invalid transitions and state configurations at build time.
* **Zero External Dependencies**: Designed as a standalone, header-only library (tests use `doctest`).
* **Rich Transition Support**: Guards, effects (actions), automatic path computation (LCA), and self/internal transitions.
* **Unified Event Queue & Deferral**: A single, compile-time-sized ring buffer powers both explicit deferral (`defer<Event>()`) and events dispatched while the machine is already in transit (nested `dispatch`, timers, attribute changes, and `call<"name">`).
* **No Heap Allocations in the Model**: All state, attribute, and queue storage is held directly in the `HSM` instance; there are no dynamic allocations inside the state machine itself.
* **Maximum Performance**: Template-based implementation with `constexpr` evaluation and flat dispatch tables.

## Performance

Benchmarks on Apple M1 Pro (Release build, 1M iterations):

| Scenario | hsm | vs QP | vs Boost.MSM |
|----------|-------|-------|--------------|
| Simple ping-pong | 123M/s | +65% | -54% |
| Hierarchical states | 69M/s | -7% | -73% |
| Guarded transitions | 127M/s | -4% | -49% |

hsm prioritizes **safety and predictability** (thread-free scheduling, ISR-safe dispatch, compile-time validation) while maintaining competitive performance with production HSM frameworks.

## Requirements

* **C++ Compiler**: Requires a compiler with **C++20** support (e.g., Clang 14+, GCC 11+, MSVC 2019 16.11+).
* **CMake**: Version 3.16 or higher.

## Installation

Since `hsm` is a header-only library, you can simply include the `include` directory in your project.

### Using CMake (FetchContent)

You can integrate `hsm` into your CMake project using `FetchContent`:

```cmake
include(FetchContent)

FetchContent_Declare(
    hsm
    GIT_REPOSITORY https://github.com/yourusername/hsm.git
    GIT_TAG main
)
FetchContent_MakeAvailable(hsm)

# Link against the interface library
target_link_libraries(your_target PRIVATE hsm)
```

## Usage

The `hsm` namespace leverages C++20/23 features to build the state machine at compile time.

```cpp
#include <iostream>
#include "hsm/hsm.hpp"

// Forward declaration for CRTP
struct MyMachine;

// Define behaviors
void on_enter_idle(hsm::Context&, MyMachine& m, const hsm::EventBase&) {
    std::cout << "Idle (Count: " << m.counter++ << ")\\n";
}

constexpr auto model = hsm::define("CompileTimeMachine",
    hsm::initial(hsm::target("idle")),

    hsm::state("idle",
        hsm::entry(on_enter_idle),
        // Example of type-based deferral (zero overhead if not used)
        // hsm::defer<SomeEvent>(), 
        hsm::transition(
            hsm::on("NEXT"),
            hsm::target("active")
        )
    ),

    hsm::state("active",
        hsm::transition(
            hsm::on("BACK"),
            hsm::target("idle")
        )
    )
);

// Define your machine type using CRTP
struct MyMachine : hsm::HSM<model, MyMachine> {
    int counter = 0;

    // Inherit constructors
    using HSM::HSM;
};

int main() {
    // Create the HSM
    MyMachine machine;

    // String-based dispatch with compile-time mapping
    machine.dispatch("NEXT");

    return 0;
}
```

### Recommended Pattern

```cpp
Machine sm;
auto task = sm.start();     // Initialize, get engine task

sm.dispatch<"event">();     // Enqueue event (fire-and-forget)
task.resume();              // Drive engine to process events

// For timer-based machines, drive in a loop:
while (!task.done()) {
    task.resume();
}
```

### Multi-Machine Dispatch (Groups)

`hsm::Group` allows you to manage multiple heterogeneous state machines and dispatch events to them either collectively (broadcast) or individually by ID.

```cpp
#include "hsm/group.hpp"

// Define machines
struct MachineA : hsm::HSM<model1, MachineA> { using HSM::HSM; };
struct MachineB : hsm::HSM<model2, MachineB> { using HSM::HSM; };

// Create machines with IDs
MachineA m1("service_A");
MachineB m2("service_B");

// Create a group
// The group flattens the hierarchy at compile-time for O(1) broadcast performance
auto group = hsm::make_group(m1, m2);

// Start all machines and get a GroupTask driver
auto driver = group.start();

// Broadcast to ALL machines (flattened O(1) iteration)
group.dispatch(StartEvent{});
group.dispatch<"reset">(); // String literal broadcast

// Drive all machines together
driver.resume();

// Dispatch to SPECIFIC machine by ID (recursive search)
group.dispatch("service_A", PingEvent{});
group.dispatch<"ping">("service_B");
driver.resume();

// For timer-based groups, drive in a loop:
while (!driver.done()) {
    if (auto dl = driver.deadline()) {
        // Wait until earliest deadline or external wake
    }
    driver.resume();
}
```

### Runtime API

* **`hsm::HSM<model, Self, Scheduler, Clock, Signal, QueuePolicy>`**: Generates the hierarchical state machine type.
  * **`Self`**: The type inheriting from `HSM` (CRTP pattern).
  * **`Scheduler`**: Optional template for task scheduling (default: `hsm::Scheduler`).
  * **`Clock`**: Optional clock type (default: `hsm::Clock`).
  * **`Signal`**: Optional signal type (default: `hsm::Signal`).
  * **`QueuePolicy`**: Compile-time policy for the capacity of the unified internal event queue (default: `hsm::DefaultQueuePolicy<model>`).
* **Construction**: Supports optional ID, Signal, and Scheduler arguments.
  * `MyMachine("id")`
  * `MyMachine("id", parent_signal)`
* **`machine.dispatch("event_name")`**: Dispatches an event. Returns `result_t`:
  * `Processed` - Event enqueued successfully
  * `QueueFull` - Queue overflow, event rejected
  * `Deferred` - Event deferred for later processing
* **`machine.state()`**: Returns the current state path as `std::string_view`.
* **`machine.id()`**: Returns the machine's ID as `std::string_view`.

## Building and Testing

The project uses CMake. To build the library and run tests:

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
cmake --build .
ctest --output-on-failure
```

### Running Examples

Examples are built when `BUILD_EXAMPLES` is ON (default).

```bash
./examples/hsm_example
```

## Documentation

* [HSM User Guide](docs/hsm.md)
* [FreeRTOS Integration](docs/hsm_freertos.md)

## License

This project is available under the MIT License.
