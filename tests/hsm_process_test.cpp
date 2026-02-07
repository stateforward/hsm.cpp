#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <numeric>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Robust tests for HSM::process() — synchronous inline dispatch.
//
// These tests verify that process() performs real state transitions, fires
// entry/exit actions, evaluates guards, executes effects, handles completion
// transitions, and produces results identical to dispatch()+resume().
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct Go : Event<make_kind(400, Kind::Event)> {};
struct Stop : Event<make_kind(401, Kind::Event)> {};
struct Tick : Event<make_kind(402, Kind::Event)> {};
struct DataEvt : Event<make_kind(403, Kind::Event)> {
  int payload{};
};
struct Toggle : Event<make_kind(404, Kind::Event)> {};
struct Reset : Event<make_kind(405, Kind::Event)> {};

// ---------------------------------------------------------------------------
// 1. Simple two-state model with entry/exit tracing
// ---------------------------------------------------------------------------

struct TwoStateCtx {
  std::vector<std::string> trace;
  int entry_count{0};
  int exit_count{0};
};

constexpr auto two_state_model = define(
    "TS",
    initial(target("/TS/Off")),
    state("Off",
          entry([](TwoStateCtx& c) {
            c.trace.push_back("enter:Off");
            ++c.entry_count;
          }),
          exit([](TwoStateCtx& c) {
            c.trace.push_back("exit:Off");
            ++c.exit_count;
          }),
          transition(on<Go>(), target("/TS/On"))),
    state("On",
          entry([](TwoStateCtx& c) {
            c.trace.push_back("enter:On");
            ++c.entry_count;
          }),
          exit([](TwoStateCtx& c) {
            c.trace.push_back("exit:On");
            ++c.exit_count;
          }),
          transition(on<Stop>(), target("/TS/Off"))));

struct TwoStateSM : TwoStateCtx, HSM<two_state_model, TwoStateSM> {};

// ---------------------------------------------------------------------------
// 2. Pipeline model (reuses the manual drive Controller for parity checks)
// ---------------------------------------------------------------------------

struct PipeCtx {
  std::vector<std::string> trace;
  std::vector<int> buffer;
  int processed_sum{0};
};

constexpr auto pipe_model = define(
    "pipe",
    initial(target("/pipe/off")),
    state("off",
          entry([](PipeCtx& c) { c.trace.push_back("enter:off"); }),
          exit([](PipeCtx& c) { c.trace.push_back("exit:off"); }),
          transition(on<Go>(), target("/pipe/idle"))),
    state("idle",
          entry([](PipeCtx& c) { c.trace.push_back("enter:idle"); }),
          exit([](PipeCtx& c) { c.trace.push_back("exit:idle"); }),
          // Internal transition: buffer input without leaving state
          transition(on<DataEvt>(),
                     effect([](PipeCtx& c, const DataEvt& e) {
                       c.buffer.push_back(e.payload);
                       c.trace.push_back("data:" + std::to_string(e.payload));
                     })),
          transition(on<Tick>(),
                     guard([](PipeCtx& c) { return !c.buffer.empty(); }),
                     target("/pipe/busy")),
          transition(on<Reset>(),
                     effect([](PipeCtx& c) {
                       c.buffer.clear();
                       c.trace.push_back("reset");
                     }),
                     target("/pipe/idle")),
          transition(on<Stop>(), target("/pipe/off"))),
    state("busy",
          entry([](PipeCtx& c) {
            int sum = 0;
            for (auto v : c.buffer) sum += v;
            c.processed_sum += sum;
            c.buffer.clear();
            c.trace.push_back("processed:" + std::to_string(sum));
          }),
          transition(on<Stop>(), target("/pipe/off")),
          // Completion transition: auto-return to idle
          transition(target("/pipe/idle"))));

struct PipeSM : PipeCtx, HSM<pipe_model, PipeSM> {};

// ---------------------------------------------------------------------------
// 3. Guarded model: guard toggles flag, transition only on true
// ---------------------------------------------------------------------------

struct GuardCtx {
  bool flag{false};
  int transition_count{0};
};

constexpr auto guard_model = define(
    "GM",
    initial(target("/GM/S")),
    state("S",
          transition(on<Toggle>(),
                     guard([](GuardCtx& c, const Toggle&) {
                       c.flag = !c.flag;
                       return c.flag;
                     }),
                     effect([](GuardCtx& c) { ++c.transition_count; }),
                     target("/GM/S"))));

struct GuardSM : GuardCtx, HSM<guard_model, GuardSM> {};

// ---------------------------------------------------------------------------
// 4. Hierarchical model with deep entry/exit tracing
// ---------------------------------------------------------------------------

struct HierCtx {
  std::vector<std::string> trace;
};

constexpr auto hier_model = define(
    "H",
    initial(target("/H/P")),
    state("P",
          entry([](HierCtx& c) { c.trace.push_back("enter:P"); }),
          exit([](HierCtx& c) { c.trace.push_back("exit:P"); }),
          state("C1",
                entry([](HierCtx& c) { c.trace.push_back("enter:C1"); }),
                exit([](HierCtx& c) { c.trace.push_back("exit:C1"); })),
          state("C2",
                entry([](HierCtx& c) { c.trace.push_back("enter:C2"); }),
                exit([](HierCtx& c) { c.trace.push_back("exit:C2"); })),
          initial(target("/H/P/C1")),
          transition(on<Go>(), source("/H/P/C1"), target("/H/P/C2")),
          transition(on<Stop>(), source("/H/P/C2"), target("/H/P/C1"))));

struct HierSM : HierCtx, HSM<hier_model, HierSM> {};

// ---------------------------------------------------------------------------
// 5. Traffic light ring (mirrors the benchmark model but with real tracing)
// ---------------------------------------------------------------------------

struct TrafficCtx {
  std::vector<std::string> visited;
};

// Transitions use source() DSL at the Op parent level.
constexpr auto traffic_model = define(
    "TL",
    initial(target("/TL/Op")),
    state("Op",
          initial(target("/TL/Op/NS")),
          state("NS",
                state("Green",
                      entry([](TrafficCtx& c) { c.visited.push_back("NS_Green"); })),
                state("Yellow",
                      entry([](TrafficCtx& c) { c.visited.push_back("NS_Yellow"); })),
                initial(target("/TL/Op/NS/Green"))),
          state("EW",
                state("Green",
                      entry([](TrafficCtx& c) { c.visited.push_back("EW_Green"); })),
                state("Yellow",
                      entry([](TrafficCtx& c) { c.visited.push_back("EW_Yellow"); })),
                initial(target("/TL/Op/EW/Green"))),
          state("AllRed1",
                entry([](TrafficCtx& c) { c.visited.push_back("AllRed1"); })),
          state("AllRed2",
                entry([](TrafficCtx& c) { c.visited.push_back("AllRed2"); })),
          transition(on<Tick>(), source("/TL/Op/NS/Green"),
                     target("/TL/Op/NS/Yellow")),
          transition(on<Tick>(), source("/TL/Op/NS/Yellow"),
                     target("/TL/Op/AllRed1")),
          transition(on<Tick>(), source("/TL/Op/AllRed1"),
                     target("/TL/Op/EW/Green")),
          transition(on<Tick>(), source("/TL/Op/EW/Green"),
                     target("/TL/Op/EW/Yellow")),
          transition(on<Tick>(), source("/TL/Op/EW/Yellow"),
                     target("/TL/Op/AllRed2")),
          transition(on<Tick>(), source("/TL/Op/AllRed2"),
                     target("/TL/Op/NS/Green"))));

struct TrafficSM : TrafficCtx, HSM<traffic_model, TrafficSM> {};

// ---------------------------------------------------------------------------
// 6. Three-state counter model for bulk iteration tests
// ---------------------------------------------------------------------------

struct CounterCtx {
  int transitions{0};
  int a_entries{0};
  int b_entries{0};
  int c_entries{0};
};

constexpr auto counter_model = define(
    "C",
    initial(target("/C/A")),
    state("A",
          entry([](CounterCtx& c) { ++c.a_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/B"))),
    state("B",
          entry([](CounterCtx& c) { ++c.b_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/C"))),
    state("C",
          entry([](CounterCtx& c) { ++c.c_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/A"))));

struct CounterSM : CounterCtx, HSM<counter_model, CounterSM> {};

}  // namespace

// ============================================================================
// Tests
// ============================================================================

// --- Basic state transitions ---

TEST_CASE("process - transitions state on each call") {
  TwoStateSM sm;
  sm.start();
  CHECK(sm.state() == "/TS/Off");

  sm.process<Go>();
  CHECK(sm.state() == "/TS/On");

  sm.process<Stop>();
  CHECK(sm.state() == "/TS/Off");
}

TEST_CASE("process - fires entry and exit actions") {
  TwoStateSM sm;
  sm.start();
  // start enters initial state
  REQUIRE(sm.trace.size() == 1);
  CHECK(sm.trace[0] == "enter:Off");
  CHECK(sm.entry_count == 1);
  CHECK(sm.exit_count == 0);

  sm.process<Go>();
  // exit:Off, enter:On
  REQUIRE(sm.trace.size() == 3);
  CHECK(sm.trace[1] == "exit:Off");
  CHECK(sm.trace[2] == "enter:On");
  CHECK(sm.entry_count == 2);
  CHECK(sm.exit_count == 1);

  sm.process<Stop>();
  // exit:On, enter:Off
  REQUIRE(sm.trace.size() == 5);
  CHECK(sm.trace[3] == "exit:On");
  CHECK(sm.trace[4] == "enter:Off");
  CHECK(sm.entry_count == 3);
  CHECK(sm.exit_count == 2);
}

TEST_CASE("process - entry/exit counts accumulate over many cycles") {
  TwoStateSM sm;
  sm.start();

  constexpr int N = 1000;
  for (int i = 0; i < N; ++i) {
    sm.process<Go>();
    sm.process<Stop>();
  }

  CHECK(sm.state() == "/TS/Off");
  // 1 initial entry + N Go entries + N Stop entries = 1 + 2N
  CHECK(sm.entry_count == 1 + 2 * N);
  // N exits from Off + N exits from On = 2N
  CHECK(sm.exit_count == 2 * N);
  // Each cycle adds 4 trace entries (exit+enter+exit+enter)
  CHECK(sm.trace.size() == 1 + 4 * static_cast<std::size_t>(N));
}

TEST_CASE("process - unhandled event does not change state") {
  TwoStateSM sm;
  sm.start();
  CHECK(sm.state() == "/TS/Off");

  // Stop is not handled in Off state
  sm.process<Stop>();
  CHECK(sm.state() == "/TS/Off");
  // Only the initial entry, no additional actions
  CHECK(sm.trace.size() == 1);
}

// --- Guards ---

TEST_CASE("process - guard blocks transition") {
  PipeSM sm;
  sm.start();

  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");

  // Tick with empty buffer: guard rejects
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 0);
}

TEST_CASE("process - guard allows transition when condition met") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d{}; d.payload = 42;
  sm.process(d);
  CHECK(sm.buffer.size() == 1);

  sm.process<Tick>();
  // busy processes buffer and auto-returns to idle via completion
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 42);
  CHECK(sm.buffer.empty());
}

TEST_CASE("process - guard toggle produces exact 50% transition rate") {
  GuardSM sm;
  sm.start();
  CHECK(sm.state() == "/GM/S");

  constexpr int N = 10000;
  for (int i = 0; i < N; ++i) {
    sm.process<Toggle>();
  }

  // Guard toggles flag each call; transition fires when flag becomes true.
  // Odd calls (1st, 3rd, ...) set flag=true and transition.
  // Even calls (2nd, 4th, ...) set flag=false and don't transition.
  CHECK(sm.transition_count == N / 2);
}

// --- Effects with event data ---

TEST_CASE("process - effect receives event payload") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d1{}; d1.payload = 10;
  DataEvt d2{}; d2.payload = 20;
  DataEvt d3{}; d3.payload = 30;

  sm.process(d1);
  sm.process(d2);
  sm.process(d3);

  REQUIRE(sm.buffer.size() == 3);
  CHECK(sm.buffer[0] == 10);
  CHECK(sm.buffer[1] == 20);
  CHECK(sm.buffer[2] == 30);

  // Check trace
  bool found10 = false, found20 = false, found30 = false;
  for (const auto& t : sm.trace) {
    if (t == "data:10") found10 = true;
    if (t == "data:20") found20 = true;
    if (t == "data:30") found30 = true;
  }
  CHECK(found10);
  CHECK(found20);
  CHECK(found30);
}

// --- Completion transitions ---

TEST_CASE("process - completion transition fires automatically") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d{}; d.payload = 7;
  sm.process(d);

  // Tick transitions to busy, which processes and auto-returns to idle
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 7);

  // Verify the trace shows busy entry then idle re-entry
  bool found_processed = false;
  bool found_idle_after = false;
  for (std::size_t i = 0; i < sm.trace.size(); ++i) {
    if (sm.trace[i] == "processed:7") {
      found_processed = true;
      // The next trace entry after processed should be enter:idle
      if (i + 1 < sm.trace.size()) {
        found_idle_after = (sm.trace[i + 1] == "enter:idle");
      }
    }
  }
  CHECK(found_processed);
  CHECK(found_idle_after);
}

// --- Internal transitions ---

TEST_CASE("process - internal transition does not fire entry/exit") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  std::size_t trace_before = sm.trace.size();
  std::string last_entry = sm.trace.back();
  CHECK(last_entry == "enter:idle");

  DataEvt d{}; d.payload = 99;
  sm.process(d);

  // Internal transition adds only the effect trace, no exit/entry
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.trace.size() == trace_before + 1);
  CHECK(sm.trace.back() == "data:99");
}

// --- Hierarchical transitions ---

TEST_CASE("process - hierarchical entry order is outside-in") {
  HierSM sm;
  sm.start();

  // Initial: enter P, then enter C1
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "enter:P");
  CHECK(sm.trace[1] == "enter:C1");
  CHECK(sm.state() == "/H/P/C1");
}

TEST_CASE("process - hierarchical transition fires child exit/entry") {
  HierSM sm;
  sm.start();
  sm.trace.clear();

  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // C1→C2 within P: exit C1, enter C2 (P stays active)
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "exit:C1");
  CHECK(sm.trace[1] == "enter:C2");
  // P should NOT have been exited/re-entered
  for (const auto& t : sm.trace) {
    CHECK(t != "exit:P");
    CHECK(t != "enter:P");
  }
}

TEST_CASE("process - hierarchical round-trip preserves state") {
  HierSM sm;
  sm.start();

  for (int i = 0; i < 100; ++i) {
    sm.process<Go>();
    CHECK(sm.state() == "/H/P/C2");
    sm.process<Stop>();
    CHECK(sm.state() == "/H/P/C1");
  }
}

// --- Traffic light ring ---

TEST_CASE("process - traffic light completes full ring") {
  // First verify the ring works with dispatch+resume
  {
    TrafficSM sm;
    auto task = sm.start();
    REQUIRE(sm.state() == "/TL/Op/NS/Green");

    sm.dispatch(Tick{});
    task.resume();
    REQUIRE(sm.state() == "/TL/Op/NS/Yellow");

    sm.dispatch(Tick{});
    task.resume();
    // If this fails, the model itself has the bug (not process())
    CHECK(sm.state() == "/TL/Op/AllRed1");
  }

  // Now test via process()
  {
    TrafficSM sm;
    sm.start();
    REQUIRE(sm.state() == "/TL/Op/NS/Green");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/NS/Yellow");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/AllRed1");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/EW/Green");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/EW/Yellow");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/AllRed2");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/NS/Green");
  }
}

TEST_CASE("process - traffic light multiple full rings") {
  TrafficSM sm;
  sm.start();
  sm.visited.clear();

  constexpr int RINGS = 100;
  for (int r = 0; r < RINGS; ++r) {
    for (int t = 0; t < 6; ++t) {
      sm.process<Tick>();
    }
    CHECK(sm.state() == "/TL/Op/NS/Green");
  }
  // 6 entries per ring
  CHECK(sm.visited.size() == 6 * static_cast<std::size_t>(RINGS));
}

// --- Bulk iteration correctness ---

TEST_CASE("process - three-state counter over many iterations") {
  CounterSM sm;
  sm.start();

  constexpr int N = 99999;  // multiple of 3 for clean cycles
  for (int i = 0; i < N; ++i) {
    sm.process<Tick>();
  }

  // N ticks = N transitions through the ring A→B→C→A→...
  CHECK(sm.transitions == N);

  // Each state entered N/3 times during transitions, plus initial entry for A
  CHECK(sm.a_entries == 1 + N / 3);
  CHECK(sm.b_entries == N / 3);
  CHECK(sm.c_entries == N / 3);

  // After N ticks (N divisible by 3), should be back at A
  CHECK(sm.state() == "/C/A");
}

TEST_CASE("process - three-state counter non-multiple-of-3") {
  CounterSM sm;
  sm.start();

  // 5 ticks: A→B→C→A→B→C
  for (int i = 0; i < 5; ++i) {
    sm.process<Tick>();
  }
  CHECK(sm.state() == "/C/C");
  CHECK(sm.transitions == 5);
  CHECK(sm.a_entries == 1 + 1);  // initial + one return
  CHECK(sm.b_entries == 2);
  CHECK(sm.c_entries == 2);
}

// --- Parity with dispatch + resume ---

TEST_CASE("process - produces identical results to dispatch+resume") {
  // Drive two machines with the same event sequence:
  // one via process(), one via dispatch()+resume().
  // Verify they end up in the same state with the same side effects.

  auto make_events = []() {
    std::vector<int> payloads = {10, 20, 30, 40, 50};
    return payloads;
  };

  // --- process() path ---
  PipeSM sm_proc;
  sm_proc.start();
  sm_proc.process<Go>();
  for (int v : make_events()) {
    DataEvt d{}; d.payload = v;
    sm_proc.process(d);
  }
  sm_proc.process<Tick>();
  sm_proc.process<Go>();  // unhandled in idle, should be no-op
  DataEvt extra{}; extra.payload = 99;
  sm_proc.process(extra);
  sm_proc.process<Stop>();

  // --- dispatch+resume path ---
  PipeSM sm_disp;
  auto task = sm_disp.start();
  sm_disp.dispatch(Go{});
  task.resume();
  for (int v : make_events()) {
    DataEvt d{}; d.payload = v;
    sm_disp.dispatch(d);
    task.resume();
  }
  sm_disp.dispatch(Tick{});
  task.resume();
  sm_disp.dispatch(Go{});
  task.resume();
  DataEvt extra2{}; extra2.payload = 99;
  sm_disp.dispatch(extra2);
  task.resume();
  sm_disp.dispatch(Stop{});
  task.resume();

  // --- Compare results ---
  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.processed_sum == sm_disp.processed_sum);
  CHECK(sm_proc.buffer == sm_disp.buffer);
  CHECK(sm_proc.trace == sm_disp.trace);
}

TEST_CASE("process - parity with dispatch+resume: guard toggle") {
  GuardSM sm_proc;
  sm_proc.start();

  GuardSM sm_disp;
  auto task = sm_disp.start();

  constexpr int N = 500;
  for (int i = 0; i < N; ++i) {
    sm_proc.process<Toggle>();
    sm_disp.dispatch(Toggle{});
    task.resume();
  }

  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.flag == sm_disp.flag);
  CHECK(sm_proc.transition_count == sm_disp.transition_count);
}

TEST_CASE("process - parity with dispatch+resume: traffic ring") {
  TrafficSM sm_proc;
  sm_proc.start();

  TrafficSM sm_disp;
  auto task = sm_disp.start();

  constexpr int TICKS = 42;
  for (int i = 0; i < TICKS; ++i) {
    sm_proc.process<Tick>();
    sm_disp.dispatch(Tick{});
    task.resume();
  }

  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.visited == sm_disp.visited);
}

// --- Full pipeline lifecycle via process() ---

TEST_CASE("process - full pipeline lifecycle with trace verification") {
  PipeSM sm;
  sm.start();
  CHECK(sm.state() == "/pipe/off");
  CHECK(sm.trace.back() == "enter:off");

  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.trace.back() == "enter:idle");

  // Feed data
  for (int v = 1; v <= 5; ++v) {
    DataEvt d{}; d.payload = v;
    sm.process(d);
    CHECK(sm.state() == "/pipe/idle");
  }
  CHECK(sm.buffer.size() == 5);

  // Process: transitions to busy, processes, auto-returns
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 15);  // 1+2+3+4+5
  CHECK(sm.buffer.empty());

  // Reset: self-transition on idle
  sm.process<Reset>();
  CHECK(sm.state() == "/pipe/idle");

  // Power off
  sm.process<Stop>();
  CHECK(sm.state() == "/pipe/off");
  CHECK(sm.trace.back() == "enter:off");

  // Verify trace is non-trivial (proves actions actually ran)
  CHECK(sm.trace.size() > 10);
}

// --- Stress: interleaved guard/data/process cycles ---

TEST_CASE("process - stress pipeline over many batches") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  int expected_sum = 0;

  for (int batch = 1; batch <= 200; ++batch) {
    // Feed batch_size items
    int batch_sum = 0;
    for (int j = 0; j < batch % 7 + 1; ++j) {
      int val = batch * 10 + j;
      DataEvt d{}; d.payload = val;
      sm.process(d);
      batch_sum += val;
    }

    // Process
    sm.process<Tick>();
    expected_sum += batch_sum;
    CHECK(sm.state() == "/pipe/idle");
    CHECK(sm.buffer.empty());
  }

  CHECK(sm.processed_sum == expected_sum);
}

// --- Edge cases ---

TEST_CASE("process - calling process before start does not crash") {
  TwoStateSM sm;
  // process() before start() should be safe (no-op, state not initialized)
  sm.process<Go>();
  sm.process<Stop>();
  // Now start normally
  sm.start();
  CHECK(sm.state() == "/TS/Off");
  // Verify actions from start, not from the pre-start process calls
  CHECK(sm.entry_count == 1);
}

TEST_CASE("process - interleaving process and dispatch+resume") {
  PipeSM sm;
  auto task = sm.start();

  // Use process() for some events
  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");

  // Use dispatch+resume for others
  DataEvt d{}; d.payload = 77;
  sm.dispatch(d);
  task.resume();
  CHECK(sm.buffer.size() == 1);
  CHECK(sm.buffer[0] == 77);

  // Back to process
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 77);
}

// --- source() DSL tests ---

TEST_CASE("process - source() routes transitions by source state") {
  // The hier_model (section 4 above) already uses source() at the parent
  // level. This test explicitly verifies the exact pattern that was broken
  // before: multiple transitions sharing the same event at a parent level
  // with different source() specs.
  HierSM sm;
  sm.start();
  CHECK(sm.state() == "/H/P/C1");

  // Go transitions from C1 -> C2 (source="/H/P/C1")
  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // Go should NOT transition from C2 (no source="/H/P/C2" for Go)
  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // Stop transitions from C2 -> C1 (source="/H/P/C2")
  sm.process<Stop>();
  CHECK(sm.state() == "/H/P/C1");

  // Stop should NOT transition from C1 (no source="/H/P/C1" for Stop)
  sm.process<Stop>();
  CHECK(sm.state() == "/H/P/C1");
}

TEST_CASE("process - source() at parent level drives traffic ring correctly") {
  // This verifies that all 6 source()-routed transitions in the traffic
  // model fire in the correct sequence.
  TrafficSM sm;
  sm.start();
  sm.visited.clear();
  CHECK(sm.state() == "/TL/Op/NS/Green");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/NS/Yellow");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/AllRed1");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/EW/Green");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/EW/Yellow");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/AllRed2");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/NS/Green");

  // Verify entry actions fired for each visited state
  REQUIRE(sm.visited.size() == 6);
  CHECK(sm.visited[0] == "NS_Yellow");
  CHECK(sm.visited[1] == "AllRed1");
  CHECK(sm.visited[2] == "EW_Green");
  CHECK(sm.visited[3] == "EW_Yellow");
  CHECK(sm.visited[4] == "AllRed2");
  CHECK(sm.visited[5] == "NS_Green");
}
