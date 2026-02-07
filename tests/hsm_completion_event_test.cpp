#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <functional>
#include <memory>
#include <queue>

#include "hsm/hsm.hpp"

using namespace hsm;

struct Kick : hsm::Event<hsm::make_kind(100, hsm::Kind::Event)> {};
struct Next : hsm::Event<hsm::make_kind(101, hsm::Kind::Event)> {};
struct Done : hsm::Event<hsm::make_kind(102, hsm::Kind::Event)> {};

// --- Test Utilities ---

struct TestInstance {
  bool flag = false;
  int counter = 0;
};

// --- Helper Types for Models ---

static int activity_run_count = 0;
struct MyActivity {
  void operator()(Signal&, TestInstance&, const AnyEvent&) const { activity_run_count++; }
};

struct GuardedInstance {
  bool ready = false;
};

struct IsReady {
  bool operator()(GuardedInstance& i, const EventBase&) const { return i.ready; }
};

struct MakeReady {
  void operator()(Signal&, GuardedInstance& i, const EventBase&) const { i.ready = true; }
};

struct Ctx {
  int val = 0;
  Ctx() = default;
  Ctx(int v) : val(v) {}
};

struct ValIs1 {
  bool operator()(Ctx& c, const EventBase&) const { return c.val == 1; }
};
struct ValIs2 {
  bool operator()(Ctx& c, const EventBase&) const { return c.val == 2; }
};

// Guard functor whose event parameter type is a concrete event unrelated to
// CompletionEvent. When used in a completion transition, invoke_typed will
// treat it as not_invoked for CompletionEvent and resolve_completion_step
// must skip it and continue scanning the completion_chain.
struct OtherEvent : hsm::Event<hsm::make_kind(103, hsm::Kind::Event)> {};

struct TypedGuardForOtherEvent {
  bool operator()(Ctx&, const OtherEvent&) const { return true; }
};

static int activity_done = 0;
struct MixedActivity {
  void operator()(Signal&, TestInstance&, const AnyEvent&) const { activity_done++; }
};

// Instance and activity type for multi-activity completion tests
struct MultiActInstance {
  int runs = 0;
};

struct CountActivity {
  void operator()(Signal&, MultiActInstance& inst, const AnyEvent&) const { inst.runs++; }
};

// --- Models (Global Scope for CRTP) ---

constexpr auto simple_completion_model = define(
    "SimpleMachine", initial(target("/SimpleMachine/start")),
    state("start",
          transition(target("/SimpleMachine/end"))),  // Immediate completion
    state("end"));

constexpr auto activity_completion_model = define(
    "ActivityMachine", initial(target("/ActivityMachine/working")),
    state("working", activity(MyActivity{}),
          transition(target("/ActivityMachine/done"))),
    state("done"));

constexpr auto guarded_completion_model = define(
    "TriggerMachine", initial(target("/TriggerMachine/wait")),
    state("wait",
          transition(on<Kick>(), target("/TriggerMachine/wait"),
                     effect(MakeReady{})),  // Self-transition to trigger re-eval
          transition(guard(IsReady{}), target("/TriggerMachine/finished"))),
    state("finished"));

constexpr auto hierarchical_completion_model = define(
    "HierMachine", initial(target("/HierMachine/composite")),
    state("composite",
          transition(target("/HierMachine/final_dest")),  // Completion on composite
          initial(target("/HierMachine/composite/step1")),
          state("step1",
                transition(on<Next>(),
                           target("/HierMachine/composite/step2"))),
          state("step2",
                transition(on<Done>(),
                           target("/HierMachine/composite/sub_final"))),
          final("sub_final")),
    state("final_dest"));

constexpr auto choice_completion_model = define(
    "ChoiceMachine", initial(target("/ChoiceMachine/decide")),
    state("decide",
          transition(guard(ValIs1{}), target("/ChoiceMachine/path1")),
          transition(guard(ValIs2{}), target("/ChoiceMachine/path2")),
          transition(target("/ChoiceMachine/default"))),
    state("path1"), state("path2"), state("default"));

// Completion model where the first completion transition uses a guard whose
// event parameter type is a concrete event (OtherEvent) that is incompatible
// with CompletionEvent. For completion resolution, this guard must be treated
// as detail::not_invoked so that subsequent guards and the default completion
// can be considered.
constexpr auto not_invoked_completion_model = define(
    "NotInvokedMachine", initial(target("/NotInvokedMachine/decide")),
    state("decide",
          transition(guard(TypedGuardForOtherEvent{}),
                     target("/NotInvokedMachine/typed")),
          transition(guard(ValIs1{}),
                     target("/NotInvokedMachine/path1")),
          transition(target("/NotInvokedMachine/default"))),
    state("typed"), state("path1"), state("default"));

constexpr auto mixed_completion_model = define(
    "MixedMachine", initial(target("/MixedMachine/composite")),
    state("composite", activity(MixedActivity{}),
          transition(target("/MixedMachine/finished")),
          initial(target("/MixedMachine/composite/sub1")),
          state("sub1",
                transition(on<Next>(),
                           target("/MixedMachine/composite/sub_final"))),
          final("sub_final")),
    state("finished"));

// State with multiple concurrent activities: completion must wait for all
// activities in the state to finish before firing the completion transition.
constexpr auto multi_activity_completion_model = define(
    "MultiActivityMachine", initial(target("/MultiActivityMachine/working")),
    state("working",
          activity(CountActivity{}),
          activity(CountActivity{}),
          transition(target("/MultiActivityMachine/done"))),
    state("done"));

// --- Tests ---

TEST_CASE("Completion - Simple Immediate") {
  struct Machine : HSM<simple_completion_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  CHECK(sm.state() == "/SimpleMachine/end");
}

TEST_CASE("Completion - With Activity") {
  // With native coroutines, void-returning activities complete synchronously.
  // The activity runs, completes, and then the completion transition fires.
  activity_run_count = 0;

  struct Machine : TestInstance, HSM<activity_completion_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  // Activity executes synchronously and completion fires
  CHECK(sm.state() == "/ActivityMachine/done");
  CHECK(activity_run_count == 1);
}

TEST_CASE("Completion - Guarded with Trigger") {
  struct Machine : GuardedInstance, HSM<guarded_completion_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/TriggerMachine/wait");

  sm.dispatch<Kick>();
  task.resume();
  CHECK(sm.state() == "/TriggerMachine/finished");
}

TEST_CASE("Completion - Hierarchical") {
  struct Machine : HSM<hierarchical_completion_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/HierMachine/composite/step1");

  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/HierMachine/composite/step2");
  Done done;
  sm.dispatch(done);
  task.resume();
  // step2 -> sub_final.
  // sub_final is Final.
  // resolve_completion sees current is Final.
  // Bubbles up to composite.
  // Checks completion transitions on composite.
  // Found transition to final_dest.
  CHECK(sm.state() == "/HierMachine/final_dest");
}

TEST_CASE("Completion - Multiple Choices (Priority)") {
  struct Machine : Ctx, HSM<choice_completion_model, Machine> {
    explicit Machine(int v) : Ctx(v) {}
  };

  SUBCASE("Path 1") {
    Machine sm(1);
    [[maybe_unused]] auto task = sm.start();
    CHECK(sm.state() == "/ChoiceMachine/path1");
  }

  SUBCASE("Path 2") {
    Machine sm(2);
    [[maybe_unused]] auto task = sm.start();
    CHECK(sm.state() == "/ChoiceMachine/path2");
  }

  SUBCASE("Default Path") {
    Machine sm(99);
    [[maybe_unused]] auto task = sm.start();
    CHECK(sm.state() == "/ChoiceMachine/default");
  }
}

TEST_CASE("Completion - Guards with incompatible event types are treated as not_invoked") {
  struct Machine : Ctx,
                   HSM<not_invoked_completion_model, Machine> {
    explicit Machine(int v) : Ctx(v) {}
  };

  SUBCASE("First guard not invoked, second guard passes") {
    Machine sm(1);
    [[maybe_unused]] auto task = sm.start();
    CHECK(sm.state() == "/NotInvokedMachine/path1");
  }

  SUBCASE("First guard not invoked, fall back to default") {
    Machine sm(99);
    [[maybe_unused]] auto task = sm.start();
    CHECK(sm.state() == "/NotInvokedMachine/default");
  }
}

TEST_CASE("Completion - Activity and Hierarchy Mixed") {
  // With native coroutines, activities complete synchronously.
  // The activity on composite runs when we enter composite.
  activity_done = 0;

  struct Machine : TestInstance, HSM<mixed_completion_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Activity runs immediately on entry to composite
  CHECK(activity_done == 1);
  CHECK(sm.state() == "/MixedMachine/composite/sub1");

  // 1. Finish substate workflow
  sm.dispatch<Next>();
  task.resume();
  // sub_final is Final, so completion bubbles up to composite
  // Both activity is done AND in final state -> completion fires
  CHECK(sm.state() == "/MixedMachine/finished");
}

TEST_CASE("Completion - Multiple activities in single state") {
  // With native coroutines, activities execute synchronously.
  // Both activities run and complete, then completion transition fires.
  struct Machine : MultiActInstance,
                   HSM<multi_activity_completion_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  // Both activities complete synchronously, then completion fires
  CHECK(sm.runs == 2);
  CHECK(sm.state() == "/MultiActivityMachine/done");
}
