#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

#include <vector>

using namespace hsm;

// Events used to exercise the unified queue in nested-dispatch scenarios.
struct Outer : hsm::Event<hsm::make_kind(100, hsm::Kind::Event)> {};
struct Inner : hsm::Event<hsm::make_kind(101, hsm::Kind::Event)> {};

// Queue policy with a very small capacity so we can reliably trigger overflow.
struct SmallQueuePolicy {
  static constexpr std::size_t capacity = 2;
};

// Instance data used by the test machine.
struct NestedQueueInstance {
  int outer_calls{0};
  int inner_calls{0};
  std::vector<bool> nested_results;
};

struct NestedQueueMachine;  // Forward declaration for behavior signatures.

// Behaviors declared up front so they can be referenced from the model.
void nested_outer_effect(Signal&, NestedQueueMachine&, const EventBase&);
void nested_inner_effect(Signal&, NestedQueueMachine&, const EventBase&);

// Model with no defer() usage. The Outer event transitions Idle -> Running and
// performs nested dispatch of multiple Inner events while a macrostep is in
// progress. Inner events are handled in Running and counted.
constexpr auto nested_queue_model = define(
    "NestedQueueMachine",
    initial(target("/NestedQueueMachine/Idle")),
    state("Idle",
          transition(on<Outer>(),
                     target("/NestedQueueMachine/Running"),
                     effect(nested_outer_effect))),
    state("Running",
          transition(on<Inner>(),
                     target("/NestedQueueMachine/Running"),
                     effect(nested_inner_effect))));

// CRTP machine that uses the small queue policy so that nested dispatch will
// hit the queue_full() path after a couple of enqueues.
struct NestedQueueMachine
    : NestedQueueInstance,
      HSM<nested_queue_model, NestedQueueMachine, hsm::Clock, SmallQueuePolicy> {};

// Outer effect: record that it ran, then attempt several nested dispatches of
// Inner while the HSM is in transit. The unified queue should accept only the
// first queue_capacity events; subsequent dispatch<T>() calls report failure.
void nested_outer_effect(Signal&, NestedQueueMachine& m, const EventBase&) {
  m.outer_calls++;

  for (int i = 0; i < 5; ++i) {
    // In nested context (inside effect), dispatch completes immediately (fire-and-forget).
    // Check if enqueue succeeded by examining the result.
    auto result = m.template dispatch<Inner>();
    bool ok = result != hsm::QueueFull;
    m.nested_results.push_back(ok);
  }
}

// Inner effect: simply count how many Inner events were actually processed.
void nested_inner_effect(Signal&, NestedQueueMachine& m, const EventBase&) {
  m.inner_calls++;
}

TEST_CASE("Unified queue - nested dispatch without deferral") {
  static_assert(NestedQueueMachine::has_deferred_events == false,
                "Model must have no explicit defer() usage");
  static_assert(NestedQueueMachine::queue_capacity == SmallQueuePolicy::capacity,
                "Queue capacity must be taken from QueuePolicy");

  NestedQueueMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/NestedQueueMachine/Idle");
  CHECK(sm.outer_calls == 0);
  CHECK(sm.inner_calls == 0);
  CHECK(sm.nested_results.empty());

  // Dispatch Outer once. This should:
  //   - Move the machine to Running,
  //   - Enqueue up to (queue_capacity - 1) Inner events while in transit,
  //   - Drop any additional Inner dispatches and report failure.
  //
  // With commit-after semantics: head advances AFTER processing each event,
  // so during processing the current slot is still "occupied" from the queue's
  // perspective. With capacity=2, only 1 nested dispatch can succeed.
  sm.dispatch<Outer>();
  task.resume();

  CHECK(sm.state() == "/NestedQueueMachine/Running");
  CHECK(sm.outer_calls == 1);

  // We attempted 5 nested dispatches. With capacity=2 and commit-after,
  // only 1 slot is available (the other is occupied by Outer being processed).
  REQUIRE(sm.nested_results.size() == 5);
  CHECK(sm.nested_results[0] == true);   // First Inner fits
  CHECK(sm.nested_results[1] == false);  // Queue full (2 - 0 = 2 >= 2)
  CHECK(sm.nested_results[2] == false);
  CHECK(sm.nested_results[3] == false);
  CHECK(sm.nested_results[4] == false);

  // Only the enqueued Inner event should have been processed when the outer
  // macrostep drained the queue.
  CHECK(sm.inner_calls == 1);
}
