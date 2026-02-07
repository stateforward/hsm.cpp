#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

using namespace std::chrono_literals;

// --- Simulated Clock ---
// A custom clock that can be advanced programmatically for testing timer behavior.
struct SimulatedClock {
  using duration = std::chrono::milliseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<SimulatedClock>;
  static constexpr bool is_steady = true;

  static std::atomic<rep> current_time_ms;

  static time_point now() noexcept {
    auto t = current_time_ms.load();
    // std::cerr << "SimClock::now() = " << t << std::endl;
    return time_point(duration(t));
  }

  static void advance(duration d) {
    current_time_ms.fetch_add(d.count());
  }

  static void reset() {
    current_time_ms.store(0);
  }
};

std::atomic<SimulatedClock::rep> SimulatedClock::current_time_ms{0};

// --- Helper Behavior ---
struct TrackedInstance {
  std::vector<std::string> events;
  std::mutex m;
  void add_event(std::string name) {
      std::lock_guard<std::mutex> l(m);
      events.push_back(std::move(name));
  }
  bool has_event(const std::string& name) {
      std::lock_guard<std::mutex> l(m);
      if (events.empty()) return false;
      return events.back() == name;
  }
  bool events_empty() {
      std::lock_guard<std::mutex> l(m);
      return events.empty();
  }
};

void log_enter(hsm::Signal&, TrackedInstance& i, const hsm::EventBase& /*e*/) {
  i.add_event("enter");
}

// --- Models (Global Scope) ---

constexpr auto clock_test_model = define(
    "clock_test",
    initial(target("/clock_test/idle")),
    state("idle",
          transition(after([]{ return 100ms; }), target("/clock_test/done"))),
    state("done", entry(log_enter))
);

constexpr auto get_deadline = []() {
    return SimulatedClock::time_point(200ms);
};

constexpr auto at_test_model = define(
    "at_test",
    initial(target("/at_test/idle")),
    state("idle",
          transition(at(get_deadline), target("/at_test/done"))),
    state("done")
);

struct Cancel : hsm::Event<hsm::make_kind(200, hsm::Kind::Event)> {};
struct Reset : hsm::Event<hsm::make_kind(201, hsm::Kind::Event)> {};

constexpr auto clock_cancel_model = define(
    "clock_cancel", initial(target("/clock_cancel/wait")),
    state("wait",
          transition(after([]{ return 100ms; }), target("/clock_cancel/timeout")),
          transition(on<Cancel>(), target("/clock_cancel/cancelled"))),
    state("timeout"),
    state("cancelled")
);

constexpr auto clock_reset_model = define(
    "clock_reset", initial(target("/clock_reset/wait")),
    state("wait",
          transition(after([]{ return 100ms; }), target("/clock_reset/timeout")),
          transition(on<Reset>(), target("/clock_reset/wait"))), // Self-transition re-enters
    state("timeout")
);

// --- Test Cases ---

TEST_CASE("Simulated Clock - After Transition") {
  SimulatedClock::reset();
  using namespace hsm;

  {
    struct Machine : TrackedInstance,
                     HSM<clock_test_model, Machine, SimulatedClock> {};
    Machine sm;
    auto engine = sm.start();

    CHECK(sm.state() == "/clock_test/idle");

    // Advance time partially - should not transition yet
    SimulatedClock::advance(50ms);
    engine.resume();  // Let engine check timers
    CHECK(sm.state() == "/clock_test/idle");
    CHECK(sm.events_empty());

    // Advance past the threshold
    SimulatedClock::advance(60ms); // Total 110ms
    engine.resume();  // Engine fires the timer

    CHECK(sm.state() == "/clock_test/done");
    CHECK(!sm.events_empty());
    CHECK(sm.has_event("enter"));
  }
}

TEST_CASE("Simulated Clock - At Transition") {
  SimulatedClock::reset();
  using namespace hsm;

  {
    struct Machine : TrackedInstance,
                     HSM<at_test_model, Machine, SimulatedClock> {};
    Machine sm;
    auto engine = sm.start();

    CHECK(sm.state() == "/at_test/idle");

    // Current time 0, deadline 200ms.
    SimulatedClock::advance(100ms);
    engine.resume();  // Let engine check timers
    CHECK(sm.state() == "/at_test/idle");

    SimulatedClock::advance(150ms); // Total 250ms
    engine.resume();  // Engine fires the timer

    CHECK(sm.state() == "/at_test/done");
  }
}

TEST_CASE("Simulated Clock - Timer Cancellation") {
  SimulatedClock::reset();
  using namespace hsm;

  {
    struct Machine : TrackedInstance,
                     HSM<clock_cancel_model, Machine, SimulatedClock> {};
    Machine sm;
    auto engine = sm.start();

    CHECK(sm.state() == "/clock_cancel/wait");

    // Advance time partially (50ms)
    SimulatedClock::advance(50ms);
    engine.resume();  // Let engine check timers
    CHECK(sm.state() == "/clock_cancel/wait");

    // Cancel by transitioning out
    sm.dispatch(Cancel{});
    engine.resume();
    CHECK(sm.state() == "/clock_cancel/cancelled");

    // Advance time past the original timeout (another 60ms => 110ms total)
    SimulatedClock::advance(60ms);
    engine.resume();  // Let engine check - timer should be cancelled

    // Should stay cancelled, should NOT go to timeout
    CHECK(sm.state() == "/clock_cancel/cancelled");
  }
}

TEST_CASE("Simulated Clock - Timer Reset on Re-entry") {
  SimulatedClock::reset();
  using namespace hsm;

  {
    struct Machine : TrackedInstance,
                     HSM<clock_reset_model, Machine, SimulatedClock> {};
    Machine sm;
    auto engine = sm.start();

    CHECK(sm.state() == "/clock_reset/wait");

    // Advance 90ms (close to 100ms timeout)
    SimulatedClock::advance(90ms);
    engine.resume();  // Let engine check timers
    CHECK(sm.state() == "/clock_reset/wait");

    // Reset by self-transition (exits and re-enters, restarting the timer)
    sm.dispatch(Reset{});
    engine.resume();
    CHECK(sm.state() == "/clock_reset/wait");

    // Advance another 20ms (Total 110ms since start, but only 20ms since reset)
    // If reset didn't work, it would have fired at 100ms total.
    SimulatedClock::advance(20ms);
    engine.resume();  // Let engine check timers

    // Should still be waiting (new timer started at 90ms, needs 100ms from there)
    CHECK(sm.state() == "/clock_reset/wait");

    // Advance to 100ms since reset (another 80ms)
    SimulatedClock::advance(80ms);
    engine.resume();  // Engine fires the timer

    CHECK(sm.state() == "/clock_reset/timeout");
  }
}
