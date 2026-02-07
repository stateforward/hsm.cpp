#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"
#include <vector>

using namespace hsm;

// --- Basic Deferral Tests Events ---
struct EventA : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct EventB : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};
struct Go : hsm::Event<hsm::make_kind(12, hsm::Kind::Event)> {};

// --- Queue Limit Tests Events ---
struct E1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct E2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct E3 : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct Next : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct QueueGo : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct E : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};

// --- Basic Deferral Helpers ---

struct DeferralCounterInstance {
  int processed_a{0};
};

void count_a(Signal&, DeferralCounterInstance& i, const EventBase&) {
  static_cast<DeferralCounterInstance&>(i).processed_a++;
}

constexpr auto basic_defer_model = define(
    "machine", initial(target("/machine/idle")),
    state("idle", defer<EventA>(),
          transition(on<EventB>(), target("/machine/processing"))),
    state("processing", transition(on<EventA>(), target("/machine/done"))),
    state("done"));

constexpr auto hierarchy_defer_model = define(
    "machine", initial(target("/machine/p/c")),
    state("p", defer<EventA>(),
          state("c", transition(on<EventB>(), target("/machine/other")))),
    state("other", transition(on<EventA>(), target("/machine/done"))), state("done"));

constexpr auto queue_limit_model = define(
    "DeferralQueue", initial(target("/DeferralQueue/idle")),
    state("idle", defer<EventA>(),
          transition(on<Go>(), target("/DeferralQueue/processing"))),
    state("processing",
          transition(on<EventA>(), effect(count_a), target("/DeferralQueue/processing"))));

// Model without any defer() usage: deferral queue should be fully disabled.
constexpr auto no_defer_model = define(
    "NoDefer", initial(target("/NoDefer/idle")),
    state("idle", transition(on<EventA>(), target("/NoDefer/done"))),
    state("done"));

// Model that defers AnyEvent ...
struct WildEvent : hsm::Event<hsm::make_kind(13, hsm::Kind::Event)> {};

struct WildDeferInstance {
  int handled_any{0};
};

void count_any(Signal&, WildDeferInstance& inst, const EventBase&) {
  inst.handled_any++;
}

constexpr auto wildcard_defer_model = define(
    "WildcardDefer", initial(target("/WildcardDefer/Idle")),
    state("Idle", defer<AnyEvent>(),
          transition(on<Go>(), target("/WildcardDefer/Active"))),
    state("Active",
          transition(on<AnyEvent>(), effect(count_any),
                     target("/WildcardDefer/Active"))));

// --- Queue Limit Helpers ---

// Defer policies used to control queue capacity
struct Defer2Policy {
  static constexpr std::size_t capacity = 2;
};

struct Defer100Policy {
  static constexpr std::size_t capacity = 100;
};

struct CounterInstance {
  int e1_count = 0;
  int e2_count = 0;
  int e3_count = 0;
};

void countE1(Signal&, CounterInstance& i, const EventBase&) { i.e1_count++; }
void countE2(Signal&, CounterInstance& i, const EventBase&) { i.e2_count++; }
void countE3(Signal&, CounterInstance& i, const EventBase&) { i.e3_count++; }

struct OrderInstance {
  std::vector<int> order;
};

void logE1(Signal&, OrderInstance& i, const EventBase&) { i.order.push_back(1); }
void logE2(Signal&, OrderInstance& i, const EventBase&) { i.order.push_back(2); }
void logE3(Signal&, OrderInstance& i, const EventBase&) { i.order.push_back(3); }

constexpr auto defer_queue_limit_model = define(
    "DeferLimitMachine", initial(target("/DeferLimitMachine/Idle")),
    state("Idle", defer<E1, E2, E3>(),
          transition(on<Next>(), target("/DeferLimitMachine/Process"))),
    state("Process", transition(on<E1>(), target("/DeferLimitMachine/Process")),
          transition(on<E2>(), target("/DeferLimitMachine/Process")),
          transition(on<E3>(), target("/DeferLimitMachine/Process"))));

constexpr auto defer_func_model = define(
    "DeferLimitFunc", initial(target("/DeferLimitFunc/Idle")),
    state("Idle", defer<E1, E2, E3>(),
          transition(on<Next>(), target("/DeferLimitFunc/Process"))),
    state("Process",
          transition(on<E1>(), target("/DeferLimitFunc/Process"),
                     effect(countE1)),
          transition(on<E2>(), target("/DeferLimitFunc/Process"),
                     effect(countE2)),
          transition(on<E3>(), target("/DeferLimitFunc/Process"),
                     effect(countE3))));

constexpr auto large_queue_model = define(
    "LargeQueue", initial(target("/LargeQueue/Idle")),
    state("Idle", defer<E>(),
          transition(on<QueueGo>(), target("/LargeQueue/Done"))),
    state("Done", transition(on<E>(), target("/LargeQueue/Done"))));

constexpr auto defer_partial_model = define(
    "DeferPartial", initial(target("/DeferPartial/Idle")),
    state("Idle", defer<E1, E2, E3>(),
          transition(on<Next>(), target("/DeferPartial/Process/Handle"))),
    state("Process", defer<E2, E3>(),
          state("Handle",
                transition(on<E1>(),
                           target("/DeferPartial/Process/Handle"),
                           effect(logE1))),
          transition(on<QueueGo>(), target("/DeferPartial/Drain"))),
    state("Drain",
          transition(on<E1>(), target("/DeferPartial/Drain"), effect(logE1)),
          transition(on<E2>(), target("/DeferPartial/Drain"), effect(logE2)),
          transition(on<E3>(), target("/DeferPartial/Drain"), effect(logE3))));

// --- Tests ---

TEST_CASE("Deferral - Basic") {
  struct Machine : HSM<basic_defer_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Dispatch A in idle -> deferred
  sm.dispatch<EventA>();
  task.resume();
  CHECK(sm.state() == "/machine/idle");

  // Dispatch B -> switch to processing, then A re-dispatched -> switch to done
  sm.dispatch<EventB>();
  task.resume();
  CHECK(sm.state() == "/machine/done");
}

TEST_CASE("Deferral - Hierarchy") {
  struct Machine : HSM<hierarchy_defer_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  sm.dispatch<EventA>();  // Should be deferred by parent p
  task.resume();
  CHECK(sm.state() == "/machine/p/c");

  sm.dispatch<EventB>();  // Transition to other
  task.resume();
  // A re-dispatched -> done
  CHECK(sm.state() == "/machine/done");
}

TEST_CASE("Deferral - Queue limit and re-dispatch (Unified Queue)") {
  struct Machine : DeferralCounterInstance, HSM<queue_limit_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/DeferralQueue/idle");

  for (int i = 0; i < 20; ++i) {
    sm.dispatch<EventA>();
    task.resume();
  }

  sm.dispatch<Go>();
  task.resume();

  CHECK(sm.state() == "/DeferralQueue/processing");
  CHECK(sm.processed_a == Machine::queue_capacity);
}

TEST_CASE("Deferral - Disabled when no states defer") {
  struct Machine : HSM<no_defer_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(Machine::has_deferred_events == false);

  CHECK(sm.state() == "/NoDefer/idle");
  sm.dispatch<EventA>();
  task.resume();
  CHECK(sm.state() == "/NoDefer/done");
}

TEST_CASE("Deferral - AnyEvent wildcard with non-enqueueable typed events") {
  struct Machine : WildDeferInstance, HSM<wildcard_defer_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/WildcardDefer/Idle");

  for (int i = 0; i < 5; ++i) {
    sm.dispatch<WildEvent>();
    task.resume();
  }

  sm.dispatch<Go>();
  task.resume();

  CHECK(sm.state() == "/WildcardDefer/Active");
  CHECK(sm.handled_any == 0);
}

TEST_CASE("Configurable Defer Queue Size (Custom Policy)") {
  // Compile with a tiny queue size of 2
  struct Machine : HSM<defer_queue_limit_model, Machine, hsm::Clock, Defer2Policy> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/DeferLimitMachine/Idle");

  // Queue E1 -> OK (count=1)
  sm.dispatch<E1>();
  task.resume();

  // Queue E2 -> OK (count=2)
  sm.dispatch<E2>();
  task.resume();

  // Queue E3 -> Overflow! Should be dropped (count=2)
  sm.dispatch<E3>();
  task.resume();

  // Transition to Process to drain queue
  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/DeferLimitMachine/Process");
}

TEST_CASE("Configurable Defer Queue Size - Functional") {
  // MaxDeferred = 2
  struct Machine : CounterInstance,
                   HSM<defer_func_model, Machine, hsm::Clock, Defer2Policy> {};
  Machine sm;
  auto task = sm.start();

  // Defer 3 events. Queue size is 2.
  sm.dispatch<E1>();
  task.resume();
  sm.dispatch<E2>();
  task.resume();
  sm.dispatch<E3>(); // Dropped
  task.resume();

  // Transition -> Process queue
  sm.dispatch<Next>();
  task.resume();

  CHECK(sm.e1_count == 1);
  CHECK(sm.e2_count == 1);
  CHECK(sm.e3_count == 0);  // Dropped!
}

TEST_CASE("Configurable Defer Queue Size - Large") {
  // MaxDeferred = 100
  struct Machine : HSM<large_queue_model, Machine, hsm::Clock, Defer100Policy> {};
  Machine sm;
  auto task = sm.start();

  for (int i = 0; i < 50; ++i) {
    sm.dispatch<E>();
    task.resume();
  }

  sm.dispatch<QueueGo>();
  task.resume();
  CHECK(sm.state() == "/LargeQueue/Done");
}

TEST_CASE("Deferral - Ordering and preservation across state changes") {
  struct Machine : OrderInstance,
                   HSM<defer_partial_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/DeferPartial/Idle");

  // Queue E1, then E2, then E3 in that order.
  sm.dispatch<E1>();
  task.resume();
  sm.dispatch<E2>();
  task.resume();
  sm.dispatch<E3>();
  task.resume();

  // Move to Process/Handle; E1 should be processed, E2/E3 should stay deferred.
  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/DeferPartial/Process/Handle");
  CHECK(sm.order.size() == 1);
  CHECK(sm.order[0] == 1);

  // Move to Drain; this should drain the remaining deferred events in order.
  sm.dispatch<QueueGo>();
  task.resume();
  CHECK(sm.state() == "/DeferPartial/Drain");
  REQUIRE(sm.order.size() == 3);
  CHECK(sm.order[0] == 1);
  CHECK(sm.order[1] == 2);
  CHECK(sm.order[2] == 3);
}


// Moved to global scope to avoid local struct static member restrictions
struct Defer1Policy { static constexpr std::size_t capacity = 1; };

TEST_CASE("Deferral - Queue Overflow Drop Check") {
  // Use a tiny policy (capacity=1)

  struct Machine : HSM<defer_queue_limit_model, Machine, hsm::Clock, Defer1Policy> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/DeferLimitMachine/Idle");

  // Defer 1st event -> OK
  sm.dispatch<E1>();
  task.resume();
  // Dispatch returns false if deferred? Or true?
  // In hsm dispatch usually returns true if event was consumed (deferred counts as consumed).

  // Defer 2nd event -> Should Drop because cap=1.
  sm.dispatch<E2>();
  task.resume();

  // Use internals or counting to verify.
  // Since we don't have hooks for 'drop', we verify by draining.

  sm.dispatch<Next>(); // Drain
  task.resume();
  CHECK(sm.state() == "/DeferLimitMachine/Process");

  // Send a probe event or inspect state.
  // Using the model logic: if E1 was processed it stays in Process.
  // We can't easily see *which* event triggered the self-transition in the model
  // without side effects.

  // But we can check behavior if we use a model that changes state on specific events.
  // Let's rely on `defer_func_model` from the previous tests which has counters!

  struct CounterMachine : CounterInstance,
                   HSM<defer_func_model, CounterMachine, hsm::Clock, Defer1Policy> {};
  CounterMachine cm;
  auto task2 = cm.start();

  cm.dispatch<E1>(); // Stored
  task2.resume();
  cm.dispatch<E2>(); // Dropped
  task2.resume();

  cm.dispatch<Next>(); // Drain
  task2.resume();

  CHECK(cm.e1_count == 1);
  CHECK(cm.e2_count == 0); // Proof it was dropped
}

// Deferral Priority / Handoff Models

struct PriorityE1 : hsm::Event<hsm::make_kind(20, hsm::Kind::Event)> {};
struct PriorityE2 : hsm::Event<hsm::make_kind(21, hsm::Kind::Event)> {};

constexpr auto defer_handoff_model = define(
    "DeferHandoff", initial(target("/DeferHandoff/S1")),
    state("S1", defer<PriorityE1>(),
          transition(on<PriorityE2>(), target("/DeferHandoff/S2"))),
    state("S2",
          transition(on<PriorityE1>(), target("/DeferHandoff/Done"))),
    state("Done")
);

TEST_CASE("Deferral - Handoff (Recall in Next State)") {
    struct Machine : HSM<defer_handoff_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/DeferHandoff/S1");

    sm.dispatch<PriorityE1>(); // Deferred
    task.resume();
    CHECK(sm.state() == "/DeferHandoff/S1");

    sm.dispatch<PriorityE2>(); // Trans S1 -> S2. S2 should process E1.
    task.resume();
    CHECK(sm.state() == "/DeferHandoff/Done");
}
