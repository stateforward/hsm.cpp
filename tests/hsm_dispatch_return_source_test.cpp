#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Gap #25: Dispatch return value verification for source()-routed transitions.
//
// Confirms that dispatch()+resume() and process() return the expected result_t
// values when source()-routed transitions are involved: source match, source
// miss, guard fail on source match, and deferral with source match.
//
// Key contract:
// - dispatch() returns Processed on successful enqueue (always, unless queue full)
// - process() returns Processed (always — it bypasses the queue)
// - Deferred is an internal engine result visible in the queue drain path
// - Source-miss is indistinguishable from guard-fail by return value
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Events (IDs 800-809)
// ---------------------------------------------------------------------------

struct SrcEvt : Event<make_kind(800, Kind::Event)> {};
struct SrcGuardEvt : Event<make_kind(801, Kind::Event)> {};
struct SrcDeferEvt : Event<make_kind(802, Kind::Event)> {};
struct SrcBack : Event<make_kind(803, Kind::Event)> {};

// ---------------------------------------------------------------------------
// Model: SrcReturn — composite P with children A, B, C
// ---------------------------------------------------------------------------
//
// P (composite)
// ├── A (initial) — defer<SrcDeferEvt>
// ├── B
// └── C
//
// Transitions at P level:
//   SrcEvt + source(A) → B
//   SrcGuardEvt + source(A) + guard(false) → B
//   SrcEvt + source(B) → C
//   SrcBack + source(C) → A

struct SrcReturnCtx {
  int guard_calls{0};
};

constexpr auto src_return_model = define(
    "SR",
    initial(target("/SR/P")),
    state("P",
          initial(target("/SR/P/A")),
          state("A", defer<SrcDeferEvt>()),
          state("B"),
          state("C"),
          transition(on<SrcEvt>(), source("/SR/P/A"), target("/SR/P/B")),
          transition(on<SrcGuardEvt>(), source("/SR/P/A"),
                     guard([](SrcReturnCtx& c, const SrcGuardEvt&) {
                       ++c.guard_calls;
                       return false;
                     }),
                     target("/SR/P/B")),
          transition(on<SrcEvt>(), source("/SR/P/B"), target("/SR/P/C")),
          transition(on<SrcBack>(), source("/SR/P/C"), target("/SR/P/A"))));

struct SrcReturnSM : SrcReturnCtx, HSM<src_return_model, SrcReturnSM> {};

}  // namespace

// ============================================================================
// dispatch()+resume() path
// ============================================================================

TEST_CASE("source match returns Processed") {
  SrcReturnSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/SR/P/A");

  result_t r = sm.dispatch<SrcEvt>();
  CHECK(r == Processed);
  task.resume();
  CHECK(sm.state() == "/SR/P/B");
}

TEST_CASE("source miss returns Processed") {
  SrcReturnSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/SR/P/A");

  // Move to B first
  sm.dispatch<SrcEvt>();
  task.resume();
  CHECK(sm.state() == "/SR/P/B");

  // Now move to C
  sm.dispatch<SrcEvt>();
  task.resume();
  CHECK(sm.state() == "/SR/P/C");

  // SrcEvt from C: no source(C) for SrcEvt → source miss, but returns Processed
  result_t r = sm.dispatch<SrcEvt>();
  CHECK(r == Processed);  // enqueue succeeds regardless of source match
  task.resume();
  CHECK(sm.state() == "/SR/P/C");  // no state change
}

TEST_CASE("source match + guard fail returns Processed") {
  SrcReturnSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/SR/P/A");

  result_t r = sm.dispatch<SrcGuardEvt>();
  CHECK(r == Processed);  // enqueue succeeds; guard evaluated during drain
  task.resume();
  CHECK(sm.state() == "/SR/P/A");  // guard blocked the transition
  CHECK(sm.guard_calls == 1);
}

TEST_CASE("source match + defer returns Processed at enqueue") {
  SrcReturnSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/SR/P/A");

  // SrcDeferEvt is deferred in state A
  result_t r = sm.dispatch<SrcDeferEvt>();
  CHECK(r == Processed);  // enqueue succeeds; deferral happens during drain
  task.resume();
  CHECK(sm.state() == "/SR/P/A");  // still in A, event deferred
}

TEST_CASE("round-trip through all states via source-routed transitions") {
  SrcReturnSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/SR/P/A");

  // A → B
  sm.dispatch<SrcEvt>();
  task.resume();
  CHECK(sm.state() == "/SR/P/B");

  // B → C
  sm.dispatch<SrcEvt>();
  task.resume();
  CHECK(sm.state() == "/SR/P/C");

  // C → A
  sm.dispatch<SrcBack>();
  task.resume();
  CHECK(sm.state() == "/SR/P/A");

  // Full circle again
  sm.dispatch<SrcEvt>();
  task.resume();
  CHECK(sm.state() == "/SR/P/B");
}

// ============================================================================
// process() path
// ============================================================================

TEST_CASE("process source match returns Processed") {
  SrcReturnSM sm;
  sm.start();
  CHECK(sm.state() == "/SR/P/A");

  result_t r = sm.process<SrcEvt>();
  CHECK(r == Processed);
  CHECK(sm.state() == "/SR/P/B");
}

TEST_CASE("process source miss returns Processed") {
  SrcReturnSM sm;
  sm.start();
  CHECK(sm.state() == "/SR/P/A");

  // Move to C
  sm.process<SrcEvt>();  // A→B
  sm.process<SrcEvt>();  // B→C
  CHECK(sm.state() == "/SR/P/C");

  // SrcEvt from C: source miss → Processed, no state change
  result_t r = sm.process<SrcEvt>();
  CHECK(r == Processed);
  CHECK(sm.state() == "/SR/P/C");
}

TEST_CASE("process source match + guard fail returns Processed") {
  SrcReturnSM sm;
  sm.start();
  CHECK(sm.state() == "/SR/P/A");

  result_t r = sm.process<SrcGuardEvt>();
  CHECK(r == Processed);
  CHECK(sm.state() == "/SR/P/A");
  CHECK(sm.guard_calls == 1);
}

TEST_CASE("process and dispatch+resume produce same state sequence") {
  // Drive two machines with identical event sequences
  SrcReturnSM sm_proc;
  sm_proc.start();

  SrcReturnSM sm_disp;
  auto task = sm_disp.start();

  // Sequence: SrcEvt, SrcEvt, SrcBack, SrcGuardEvt, SrcEvt
  auto step = [&](auto dispatch_fn, auto process_fn) {
    dispatch_fn();
    task.resume();
    process_fn();
    CHECK(sm_proc.state() == sm_disp.state());
  };

  step([&] { sm_disp.dispatch<SrcEvt>(); }, [&] { sm_proc.process<SrcEvt>(); });
  step([&] { sm_disp.dispatch<SrcEvt>(); }, [&] { sm_proc.process<SrcEvt>(); });
  step([&] { sm_disp.dispatch<SrcBack>(); }, [&] { sm_proc.process<SrcBack>(); });
  step([&] { sm_disp.dispatch<SrcGuardEvt>(); },
       [&] { sm_proc.process<SrcGuardEvt>(); });
  step([&] { sm_disp.dispatch<SrcEvt>(); }, [&] { sm_proc.process<SrcEvt>(); });

  CHECK(sm_proc.state() == "/SR/P/B");
  CHECK(sm_disp.state() == "/SR/P/B");
}
