#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>

#include "hsm/hsm.hpp"

using namespace hsm;

// Basic Signal propagation tests exercise the shared-parent chaining and
// the ability to stop nested schedulers via a single parent signal.

TEST_CASE("Signal - is_set propagates through parent chain") {
  Signal root;
  Signal child;

  // Initially neither is set.
  CHECK_FALSE(root.is_set());
  CHECK_FALSE(child.is_set());

  // Attach child to root and verify propagation.
  child.reset(&root);
  CHECK_FALSE(child.is_set());

  root.set();
  CHECK(root.is_set());
  CHECK(child.is_set());
}

TEST_CASE("Signal - reset clears local flag and parent link") {
  Signal root;
  Signal child;

  root.set();
  child.reset(&root);
  CHECK(child.is_set());

  // Reset with no parent: should clear and break propagation.
  child.reset(nullptr);
  CHECK_FALSE(child.is_set());
}

// --- Compile-time activity/timer counting tests ---
// These verify that the HSM correctly calculates max_concurrent_tasks
// based on model structure (activities and timers along state paths).

namespace {

// 1) Siblings-only activities: N siblings each with one activity under a
// single parent. At most one sibling can be active at a time, so the maximum
// number of concurrent tasks should be 1 even though total_activity_count == N.
constexpr auto sibling_activities_model = define(
    "SiblingsOnly",
    initial(target("/SiblingsOnly/Parent/A")),
    state("Parent",
          state("A", activity([] {})),
          state("B", activity([] {})),
          state("C", activity([] {}))));

struct SiblingActivitiesMachine
    : HSM<sibling_activities_model, SiblingActivitiesMachine> {};

static_assert(SiblingActivitiesMachine::total_activity_count == 3,
              "SiblingsOnly model should have one activity per sibling state");
static_assert(SiblingActivitiesMachine::total_timer_count == 0,
              "SiblingsOnly model should have no timers");
static_assert(SiblingActivitiesMachine::max_concurrent_tasks == 1,
              "Only one sibling state's activity can be active at a time");

// 2) Nested activities: root, intermediate, and leaf each define an activity.
// Along the deepest path all three activities can be active together.
constexpr auto nested_activities_model = define(
    "NestedActivities",
    initial(target("/NestedActivities/Root/Child/Leaf")),
    state("Root",
          activity([] {}),
          state("Child",
                activity([] {}),
                state("Leaf", activity([] {})))));

struct NestedActivitiesMachine
    : HSM<nested_activities_model, NestedActivitiesMachine> {};

static_assert(NestedActivitiesMachine::total_activity_count == 3,
              "NestedActivities model should have three activities in total");
static_assert(NestedActivitiesMachine::total_timer_count == 0,
              "NestedActivities model should have no timers");
static_assert(NestedActivitiesMachine::max_concurrent_tasks == 3,
              "All three activities can be active along the deepest path");

// 3) Timers plus activities along a single root→leaf path: an ancestor state
// has a timer transition, and the leaf has both a timer and an activity.
// Max concurrency equals the sum of those timers plus the activity.
struct TimerActivityInstance {
};

constexpr auto timers_plus_activities_model = define(
    "TimersAndActivities",
    initial(target("/TimersAndActivities/Parent/Leaf")),
    state("Parent",
          // Parent-level timer
          transition(after([](TimerActivityInstance &) {
                      return std::chrono::milliseconds(1);
                    }),
                    target("/TimersAndActivities/Parent/Leaf")),
          state("Leaf",
                // Leaf-level activity
                activity([] {}),
                // Leaf-level timer
                transition(after([](TimerActivityInstance &) {
                            return std::chrono::milliseconds(1);
                          }),
                          target("/TimersAndActivities/Done")))),
    state("Done"));

struct TimersAndActivitiesMachine
    : TimerActivityInstance,
      HSM<timers_plus_activities_model, TimersAndActivitiesMachine> {};

static_assert(TimersAndActivitiesMachine::total_activity_count == 1,
              "TimersAndActivities model should have one activity on the leaf state");
static_assert(TimersAndActivitiesMachine::total_timer_count == 2,
              "TimersAndActivities model should have two timers (parent + leaf)");
static_assert(TimersAndActivitiesMachine::max_concurrent_tasks == 3,
              "Along the Parent/Leaf path, two timers plus one activity can be active");

// --- Runtime test instance and activities ---

struct RuntimeTestInstance {
  int counter = 0;
  bool coro_started = false;
  bool coro_finished = false;
};

struct CounterActivity {
  void operator()(Signal&, RuntimeTestInstance& i, const AnyEvent&) const {
    i.counter++;
  }
};

struct CoroCounterActivity {
  ActivityTask operator()(Signal&, RuntimeTestInstance& i, const AnyEvent&) const {
    i.coro_started = true;
    i.counter++;
    i.coro_finished = true;
    co_return;
  }
};

constexpr auto counter_activity_model = define(
    "ActivityTest",
    initial(target("/ActivityTest/Active")),
    state("Active", activity(CounterActivity{})));

constexpr auto coro_activity_model = define(
    "CoroTest",
    initial(target("/CoroTest/Active")),
    state("Active", activity(CoroCounterActivity{})));

} // namespace

// Runtime tests for activity execution
TEST_CASE("Activity execution with native coroutines") {
  struct Machine : RuntimeTestInstance, HSM<counter_activity_model, Machine> {};
  Machine sm;
  sm.start();

  // Activity should have executed immediately
  CHECK(sm.counter == 1);
  CHECK(sm.state() == "/ActivityTest/Active");
}

TEST_CASE("Coroutine activity execution") {
  struct Machine : RuntimeTestInstance, HSM<coro_activity_model, Machine> {};
  Machine sm;
  sm.start();

  CHECK(sm.coro_started);
  CHECK(sm.coro_finished);
  CHECK(sm.counter == 1);
}
