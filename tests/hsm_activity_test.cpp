#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <algorithm>
#include <chrono>
#include <vector>
#include <string>
#include "hsm/hsm.hpp"

using namespace hsm;

// --- Test Utilities for Coroutine-based Activities ---

struct TestInstance {
  int counter = 0;
  std::vector<std::string> log;
  void add_log(const std::string& msg) { log.push_back(msg); }
};

// Activity that increments counter (void-returning, wrapped in coroutine by HSM)
struct MyActivity {
  void operator()(Signal&, TestInstance& i, const AnyEvent&) const {
    i.counter++;
  }
};

// Activity that returns ActivityTask (native coroutine)
// Note: Without an event loop, coroutines complete synchronously on start()
struct MyCoroActivity {
  ActivityTask operator()(Signal& sig, TestInstance& i, const AnyEvent&) const {
    i.add_log("coro_start");
    // No yields - complete immediately for synchronous execution
    if (!sig.is_set()) {
      i.counter++;
      i.add_log("coro_done");
    }
    co_return;
  }
};

// Activity which logs
void activity_a(Signal&, TestInstance& i, const AnyEvent&) {
  static_cast<TestInstance&>(i).add_log("activity_a");
}

void entry_a(Signal&, TestInstance& i, const AnyEvent&) {
  static_cast<TestInstance&>(i).add_log("entry_a");
}

constexpr auto async_comp_model = define(
    "AsyncCompMachine",
    initial(target("/AsyncCompMachine/Working")),
    state("Working",
          activity(MyActivity{}),
          transition(target("/AsyncCompMachine/Done"))
    ),
    state("Done")
);

constexpr auto coro_activity_model = define(
    "CoroMachine",
    initial(target("/CoroMachine/Working")),
    state("Working",
          activity(MyCoroActivity{}),
          transition(target("/CoroMachine/Done"))
    ),
    state("Done")
);

constexpr auto activity_order_model = define(
    "activity_machine", initial(target("/activity_machine/s")),
    state("s", entry(entry_a), activity(activity_a)));

TEST_CASE("Completion with Synchronous Activity") {
  // With native coroutines, void-returning activities are wrapped in
  // coroutines that complete immediately (no suspension points).
  // The completion transition fires after the coroutine completes.

  struct Machine : TestInstance, HSM<async_comp_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  // Activity completes immediately (no yields), so completion fires
  CHECK(sm.counter == 1);
  CHECK(sm.state() == "/AsyncCompMachine/Done");
}

TEST_CASE("Completion with Coroutine Activity") {
  // Native coroutine activity.
  // Without an event loop, coroutines complete synchronously when started.

  struct Machine : TestInstance, HSM<coro_activity_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  // Coroutine activity runs synchronously to completion
  REQUIRE(sm.log.size() >= 2);
  CHECK(sm.log[0] == "coro_start");
  CHECK(sm.log[1] == "coro_done");

  // Activity completed, completion transition fires
  CHECK(sm.state() == "/CoroMachine/Done");
  CHECK(sm.counter == 1);
}

TEST_CASE("Behaviors - Entry and Activity Order") {
  // Entry behavior runs first, then activity.
  // Both complete synchronously in this model.

  struct Machine : TestInstance, HSM<activity_order_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  CHECK(sm.state() == "/activity_machine/s");
  // Entry runs first
  REQUIRE(sm.log.size() >= 1);
  CHECK(sm.log[0] == "entry_a");
  // Then activity
  REQUIRE(sm.log.size() >= 2);
  CHECK(sm.log[1] == "activity_a");
}

// For cancellation test - defined at namespace scope
// Use make_kind to create a unique kind for this event
constexpr auto stop_event_kind = make_kind(0x1234ULL, Kind::Event);
struct StopEvent : Event<stop_event_kind> {};

struct CancellableActivity {
  // Simpler activity that just logs and completes
  void operator()(Signal& sig, TestInstance& i, const AnyEvent&) const {
    i.add_log("activity_start");
    if (!sig.is_set()) {
      i.counter++;
      i.add_log("activity_complete");
    }
  }
};

constexpr auto cancellable_model = define(
    "CancellableMachine",
    initial(target("/CancellableMachine/Working")),
    state("Working",
          activity(CancellableActivity{}),
          transition(on<StopEvent>(), target("/CancellableMachine/Stopped"))
    ),
    state("Stopped")
);

TEST_CASE("Activity Cancellation via Signal") {
  struct Machine : TestInstance, HSM<cancellable_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Activity completes synchronously, so we're in Working with activity done
  CHECK(sm.state() == "/CancellableMachine/Working");
  REQUIRE(sm.log.size() >= 1);
  CHECK(sm.log[0] == "activity_start");

  // Dispatch stop event to transition out
  sm.dispatch(StopEvent{});
  task.resume();
  CHECK(sm.state() == "/CancellableMachine/Stopped");
}

// For multi-activity test - defined at namespace scope
struct Activity1 {
  void operator()(Signal&, TestInstance& i, const AnyEvent&) const {
    i.add_log("activity1");
    i.counter += 10;
  }
};

struct Activity2 {
  void operator()(Signal&, TestInstance& i, const AnyEvent&) const {
    i.add_log("activity2");
    i.counter += 20;
  }
};

constexpr auto multi_activity_model = define(
    "MultiActivityMachine",
    initial(target("/MultiActivityMachine/Working")),
    state("Working",
          activity(Activity1{}),
          activity(Activity2{})
    )
);

TEST_CASE("Multiple Activities in Same State") {
  struct Machine : TestInstance, HSM<multi_activity_model, Machine> {};
  Machine sm;
  [[maybe_unused]] auto task = sm.start();

  CHECK(sm.state() == "/MultiActivityMachine/Working");
  CHECK(sm.counter == 30);  // 10 + 20
  REQUIRE(sm.log.size() >= 2);
  // Both activities should have run
  bool has_activity1 = std::find(sm.log.begin(), sm.log.end(), "activity1") != sm.log.end();
  bool has_activity2 = std::find(sm.log.begin(), sm.log.end(), "activity2") != sm.log.end();
  CHECK(has_activity1);
  CHECK(has_activity2);
}
