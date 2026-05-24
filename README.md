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

Benchmarks on Apple M3 Max (16-core), macOS 15.1, Apple clang 16.0.0, Release build, `--iterations=1000000`, `--warmup=1000` (run on 2026-02-20).
Throughput is in **millions of transitions per second (M/s)**. For the guarded scenario, the table shows **Dispatch/Transition** M/s because only every other dispatch transitions.

Scenario: Simple ping-pong (A<->B, no guards)

| Library | M/s |
|---------|-----|
| hsm | 6211.2 |
| vanilla_switch | 6493.5 |
| vanilla_fp | 441.1 |
| sml | 1394.7 |
| boost_msm | 266.4 |
| hfsm2 | 138.9 |
| qp | 76.1 |

Scenario: Hierarchical parent/child (P/C1/C2 with entry/exit)

| Library | M/s |
|---------|-----|
| hsm | 6557.4 |
| vanilla_switch | 7246.4 |
| vanilla_fp | 450.9 |
| sml | 1512.9 |
| boost_msm | 273.4 |
| hfsm2 | 135.3 |
| qp | 74.2 |

Scenario: Deep hierarchy (L1/L2/L3a<->L3b)

| Library | M/s |
|---------|-----|
| hsm | 6825.9 |
| vanilla_switch | 7434.9 |
| vanilla_fp | 439.7 |
| sml | 1465.2 |
| boost_msm | 265.3 |
| hfsm2 | 136.3 |
| qp | 73.4 |

Scenario: Guarded transition (every other dispatch transitions)

| Library | Dispatch/Transition M/s |
|---------|--------------------------|
| hsm | 516.3/258.1 |
| vanilla_switch | 506.3/253.2 |
| vanilla_fp | 419.8/209.9 |
| sml | 499.5/249.8 |
| boost_msm | 248.9/124.4 |
| hfsm2 | 391.5/195.8 |
| qp | 131.1/65.5 |

Scenario: Traffic light controller (complex hierarchy)

| Library | M/s |
|---------|-----|
| hsm | 1091.1 |
| hsm_threaded | 7.7 |
| vanilla_switch | 691.6 |
| vanilla_fp | 446.8 |
| sml | 482.7 |
| boost_msm | 197.1 |
| hfsm2 | 179.0 |
| qp | 66.5 |

hsm_threaded is the threaded traffic-light variant included in the benchmark harness (not a separate library).

Compile-time benchmarks (wall-clock compile time for equivalent models in `bench/compile_time`, hsm vs SML), same machine/toolchain, `-O2`, 5 runs (2026-02-20):

| Model size | hsm avg (min/max) | sml avg (min/max) |
|------------|--------------------|-------------------|
| small (2 states, 2 transitions) | 0.645s (0.553/0.903) | 0.069s (0.067/0.071) |
| medium (traffic light, 10 states, 6 transitions) | 0.834s (0.816/0.851) | 0.078s (0.073/0.084) |
| large (20 states, 22 transitions, deep hierarchy) | 2.906s (2.798/3.101) | 0.103s (0.100/0.107) |

hsm prioritizes **safety and predictability** (thread-free scheduling, ISR-safe dispatch, compile-time validation) while maintaining competitive performance against established HSM frameworks and hand-written baselines.

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
* **`machine.dispatch("event_name")`**: Dispatches an event and returns no result.
  Drive the machine task (`task.resume()` / `task.await()`) to observe completion.
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
