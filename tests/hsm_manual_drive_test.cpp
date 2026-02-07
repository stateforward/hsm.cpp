#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>
#include <string>

#include "hsm/hsm.hpp"

using namespace hsm;

namespace {

// --- Events ---

struct PowerOn : Event<make_kind(300, Kind::Event)> {};
struct PowerOff : Event<make_kind(301, Kind::Event)> {};
struct Input : Event<make_kind(302, Kind::Event)> {
  int value{};
};
struct Process : Event<make_kind(303, Kind::Event)> {};
struct Flush : Event<make_kind(304, Kind::Event)> {};

// --- Instance ---

struct Controller {
  std::vector<std::string> trace;
  std::vector<int> buffer;
  int processed_sum{0};
};

// --- Model: a small pipeline controller ---
//
//  [off] --PowerOn--> [idle] --Input--> [idle] (buffer input)
//                       |                  |
//                    Process            Flush
//                       |                  |
//                       v                  v
//                    [busy]             [idle] (clear buffer)
//                       |
//                    (auto)
//                       |
//                       v
//                    [idle]
//
//  Any state --PowerOff--> [off]

constexpr auto model = define(
    "ctl",
    initial(target("/ctl/off")),

    state("off",
          entry([](Controller& c) { c.trace.push_back("enter:off"); }),
          exit([](Controller& c) { c.trace.push_back("exit:off"); }),
          transition(on<PowerOn>(), target("/ctl/idle"))),

    state("idle",
          entry([](Controller& c) { c.trace.push_back("enter:idle"); }),
          exit([](Controller& c) { c.trace.push_back("exit:idle"); }),
          // Internal transition: buffer input without leaving state
          transition(on<Input>(),
                     effect([](Controller& c, const Input& e) {
                       c.buffer.push_back(e.value);
                       c.trace.push_back("input:" + std::to_string(e.value));
                     })),
          transition(on<Process>(),
                     guard([](Controller& c) { return !c.buffer.empty(); }),
                     target("/ctl/busy")),
          transition(on<Flush>(),
                     effect([](Controller& c) {
                       c.buffer.clear();
                       c.trace.push_back("flushed");
                     }),
                     target("/ctl/idle")),
          transition(on<PowerOff>(), target("/ctl/off"))),

    state("busy",
          entry([](Controller& c) {
            int sum = 0;
            for (auto v : c.buffer) sum += v;
            c.processed_sum += sum;
            c.buffer.clear();
            c.trace.push_back("processed:" + std::to_string(sum));
          }),
          transition(on<PowerOff>(), target("/ctl/off")),
          // Completion transition: auto-return to idle when done
          transition(target("/ctl/idle"))));

struct Machine : Controller, HSM<model, Machine> {};

}  // namespace

// ============================================================================
// Manual driving: single-threaded, no OS primitives, no scheduler.
//
// The pattern is:
//   1. sm.start()      -> returns a Task (coroutine handle)
//   2. sm.dispatch(e)  -> enqueues event (non-blocking)
//   3. task.resume()   -> engine processes one batch of queued events
//   4. Inspect state, repeat
//
// This is cooperative scheduling. You own the run loop.
// ============================================================================

TEST_CASE("Manual drive - basic start/dispatch/resume cycle") {
  Machine sm;

  // 1. Start the engine. This enters the initial state synchronously.
  auto task = sm.start();
  CHECK(sm.state() == "/ctl/off");

  // 2. Dispatch an event. This only enqueues - state is unchanged.
  sm.dispatch(PowerOn{});
  CHECK(sm.state() == "/ctl/off");  // still off!

  // 3. Resume the engine task. Now the event is processed.
  task.resume();
  CHECK(sm.state() == "/ctl/idle");
}

TEST_CASE("Manual drive - step-by-step pipeline") {
  Machine sm;
  auto task = sm.start();

  // Power on
  sm.dispatch(PowerOn{});
  task.resume();
  CHECK(sm.state() == "/ctl/idle");

  // Feed inputs one at a time
  Input i1{}; i1.value = 10;
  Input i2{}; i2.value = 20;
  Input i3{}; i3.value = 30;

  sm.dispatch(i1);
  task.resume();
  CHECK(sm.state() == "/ctl/idle");  // internal transition, no state change
  CHECK(sm.buffer.size() == 1);

  sm.dispatch(i2);
  task.resume();
  CHECK(sm.buffer.size() == 2);

  sm.dispatch(i3);
  task.resume();
  CHECK(sm.buffer.size() == 3);

  // Process: transitions to busy, which processes and auto-returns to idle
  sm.dispatch(Process{});
  task.resume();
  CHECK(sm.state() == "/ctl/idle");  // busy -> idle via completion
  CHECK(sm.processed_sum == 60);     // 10 + 20 + 30
  CHECK(sm.buffer.empty());
}

TEST_CASE("Manual drive - guard blocks transition until precondition met") {
  Machine sm;
  auto task = sm.start();

  sm.dispatch(PowerOn{});
  task.resume();

  // Process with empty buffer - guard rejects, stays idle
  sm.dispatch(Process{});
  task.resume();
  CHECK(sm.state() == "/ctl/idle");
  CHECK(sm.processed_sum == 0);

  // Add data, then process succeeds
  Input i{}; i.value = 42;
  sm.dispatch(i);
  task.resume();

  sm.dispatch(Process{});
  task.resume();
  CHECK(sm.state() == "/ctl/idle");
  CHECK(sm.processed_sum == 42);
}

TEST_CASE("Manual drive - polling loop pattern") {
  Machine sm;
  auto task = sm.start();

  sm.dispatch(PowerOn{});
  task.resume();

  // Simulate a polling loop: feed data, process, repeat
  for (int batch = 1; batch <= 3; ++batch) {
    // Feed batch
    for (int j = 1; j <= batch; ++j) {
      Input i{}; i.value = batch * 10 + j;
      sm.dispatch(i);
      task.resume();
    }

    // Process batch
    sm.dispatch(Process{});
    task.resume();
    CHECK(sm.state() == "/ctl/idle");
    CHECK(sm.buffer.empty());
  }

  // Batch 1: 11            = 11
  // Batch 2: 21 + 22       = 43
  // Batch 3: 31 + 32 + 33  = 96
  // Total: 150
  CHECK(sm.processed_sum == 150);
}

TEST_CASE("Manual drive - full lifecycle with trace") {
  Machine sm;
  auto task = sm.start();

  CHECK(sm.trace.back() == "enter:off");

  sm.dispatch(PowerOn{});
  task.resume();
  CHECK(sm.trace.back() == "enter:idle");

  Input i{}; i.value = 5;
  sm.dispatch(i);
  task.resume();
  CHECK(sm.trace.back() == "input:5");

  sm.dispatch(Process{});
  task.resume();
  // busy entry runs then completion returns to idle
  // trace should contain: processed:5, then enter:idle
  bool found_processed = false;
  for (const auto& entry : sm.trace) {
    if (entry == "processed:5") found_processed = true;
  }
  CHECK(found_processed);
  CHECK(sm.state() == "/ctl/idle");

  sm.dispatch(PowerOff{});
  task.resume();
  CHECK(sm.state() == "/ctl/off");
  CHECK(sm.trace.back() == "enter:off");
}

TEST_CASE("Manual drive - task.done() reflects engine lifetime") {
  Machine sm;
  auto task = sm.start();

  // Engine is alive (suspended, waiting for events)
  CHECK_FALSE(task.done());
  CHECK(task.joinable());

  // Drive some events - engine stays alive
  sm.dispatch(PowerOn{});
  task.resume();
  CHECK_FALSE(task.done());

  Input i{}; i.value = 1;
  sm.dispatch(i);
  task.resume();
  CHECK_FALSE(task.done());

  // Engine persists across many resumes
  sm.dispatch(Process{});
  task.resume();
  CHECK_FALSE(task.done());

  sm.dispatch(PowerOff{});
  task.resume();
  CHECK_FALSE(task.done());
}

TEST_CASE("Manual drive - multiple events between resumes") {
  Machine sm;
  auto task = sm.start();

  sm.dispatch(PowerOn{});
  task.resume();

  // Queue multiple inputs before resuming
  Input i1{}; i1.value = 1;
  Input i2{}; i2.value = 2;
  Input i3{}; i3.value = 3;
  sm.dispatch(i1);
  sm.dispatch(i2);
  sm.dispatch(i3);

  // Single resume processes all queued events
  task.resume();
  CHECK(sm.buffer.size() == 3);
  CHECK(sm.buffer[0] == 1);
  CHECK(sm.buffer[1] == 2);
  CHECK(sm.buffer[2] == 3);
}

TEST_CASE("Manual drive - resume with no pending events is harmless") {
  Machine sm;
  auto task = sm.start();

  sm.dispatch(PowerOn{});
  task.resume();
  CHECK(sm.state() == "/ctl/idle");

  // Extra resumes with nothing queued - no crash, no state change
  task.resume();
  task.resume();
  task.resume();
  CHECK(sm.state() == "/ctl/idle");
}
