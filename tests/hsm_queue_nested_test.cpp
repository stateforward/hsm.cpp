#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

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
// hit the bounded queue limit after a couple of enqueues.
struct NestedQueueMachine
    : NestedQueueInstance,
      HSM<nested_queue_model, NestedQueueMachine, hsm::Clock, SmallQueuePolicy> {};

// Outer effect: record that it ran, then attempt several nested dispatches of
// Inner while the HSM is in transit. The unified queue should accept only the
// first queue_capacity events; subsequent dispatch<T>() calls have no public
// result and excess events are not processed.
void nested_outer_effect(Signal&, NestedQueueMachine& m, const EventBase&) {
  m.outer_calls++;

  for (int i = 0; i < 5; ++i) {
    m.template dispatch<Inner>();
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

  // Dispatch Outer once. This should:
  //   - Move the machine to Running,
  //   - Enqueue up to (queue_capacity - 1) Inner events while in transit,
  //   - Drop any additional Inner dispatches without a dispatch result.
  //
  // With commit-after semantics: head advances AFTER processing each event,
  // so during processing the current slot is still "occupied" from the queue's
  // perspective. With capacity=2, only 1 nested dispatch can succeed.
  sm.dispatch<Outer>();
  task.resume();

  CHECK(sm.state() == "/NestedQueueMachine/Running");
  CHECK(sm.outer_calls == 1);

  // Only the enqueued Inner event should have been processed when the outer
  // macrostep drained the queue.
  CHECK(sm.inner_calls == 1);
}
