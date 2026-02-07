#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Bug Report Test: Internal Transitions at Composite State Not Inherited
// by Sub-States When Activity Is Running
//
// Per UML HSM semantics, internal transitions (transitions with effect() but
// no target()) defined at a composite state level should be executed when the
// HSM is in any nested sub-state, including sub-states with running activities.
//
// With native coroutines, activities execute synchronously (no suspension).
// ============================================================================

// --- Events ---

struct E_Query : Event<make_kind(1, Kind::Event)> {
  int* result = nullptr;
};
struct E_GoToProcessing : Event<make_kind(2, Kind::Event)> {};
struct E_GoToIdle : Event<make_kind(3, Kind::Event)> {};
struct E_InternalAtComposite : Event<make_kind(4, Kind::Event)> {};

// --- Test Instance ---

struct Counters {
  int entry_Active = 0;
  int exit_Active = 0;
  int entry_Idle = 0;
  int exit_Idle = 0;
  int entry_Processing = 0;
  int exit_Processing = 0;
  int effect_query = 0;
  int effect_internal = 0;
  int activity_started = 0;
  int activity_completed = 0;

  void reset() { *this = Counters{}; }
};

struct Spy : hsm::unit_instance {
  Counters counters;
  std::vector<std::string> log;

  void clear() {
    counters.reset();
    log.clear();
  }

  void on_entry_Active() {
    counters.entry_Active++;
    log.push_back("entry_Active");
  }
  void on_exit_Active() {
    counters.exit_Active++;
    log.push_back("exit_Active");
  }
  void on_entry_Idle() {
    counters.entry_Idle++;
    log.push_back("entry_Idle");
  }
  void on_exit_Idle() {
    counters.exit_Idle++;
    log.push_back("exit_Idle");
  }
  void on_entry_Processing() {
    counters.entry_Processing++;
    log.push_back("entry_Processing");
  }
  void on_exit_Processing() {
    counters.exit_Processing++;
    log.push_back("exit_Processing");
  }
  void on_effect_query() {
    counters.effect_query++;
    log.push_back("effect_query");
  }
  void on_effect_internal() {
    counters.effect_internal++;
    log.push_back("effect_internal");
  }
  void on_activity_start() {
    counters.activity_started++;
    log.push_back("activity_started");
  }
  void on_activity_complete() {
    counters.activity_completed++;
    log.push_back("activity_completed");
  }
};

// Effect handler that receives event data
void handle_query(Signal&, Spy& spy, const E_Query& e) {
  spy.on_effect_query();
  if (e.result) {
    *e.result = 42;
  }
}

// Activity for Processing state
void do_processing(Signal&, Spy& spy, const AnyEvent&) {
  spy.on_activity_start();
  // Simulate work - in real test this would be async
  spy.on_activity_complete();
}

// ============================================================================
// Model Definition
// ============================================================================
// Structure:
// Root
// └── Active (composite)
//     ├── Idle (leaf) - initial
//     └── Processing (leaf) - has activity
//
// Internal transition E_Query at Active level should work from both sub-states.
// ============================================================================

static constexpr auto inheritance_model = define(
    "Root",
    initial(target("/Root/Active/Idle")),

    state("Active",
          entry(&Spy::on_entry_Active),
          exit(&Spy::on_exit_Active),

          // Internal transition at composite level - should work from ANY
          // sub-state per UML semantics
          transition(on<E_Query>(), effect(handle_query)),
          transition(on<E_InternalAtComposite>(), effect(&Spy::on_effect_internal)),

          initial(target("/Root/Active/Idle")),

          state("Idle",
                entry(&Spy::on_entry_Idle),
                exit(&Spy::on_exit_Idle),
                transition(on<E_GoToProcessing>(),
                           target("/Root/Active/Processing"))),

          state("Processing",
                entry(&Spy::on_entry_Processing),
                exit(&Spy::on_exit_Processing),
                activity(do_processing),
                transition(on<E_GoToIdle>(), target("/Root/Active/Idle")))));

// ============================================================================
// TEST CASES
// ============================================================================

TEST_CASE("Internal transition inheritance - basic (no activity)") {
  struct Machine : Spy, HSM<inheritance_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  SUBCASE("Internal transition works from Idle sub-state") {
    CHECK(sm.state() == "/Root/Active/Idle");
    sm.clear();

    // Dispatch query event while in Idle
    int result = -1;
    E_Query query;
    query.result = &result;
    sm.dispatch(query);
    task.resume();

    // Effect should run
    CHECK(result == 42);
    CHECK(sm.counters.effect_query == 1);

    // No state changes (internal transition)
    CHECK(sm.state() == "/Root/Active/Idle");
    CHECK(sm.counters.exit_Idle == 0);
    CHECK(sm.counters.entry_Idle == 0);
    CHECK(sm.counters.exit_Active == 0);
    CHECK(sm.counters.entry_Active == 0);

    CHECK(sm.log == std::vector<std::string>{"effect_query"});
  }

  SUBCASE("Generic internal transition works from Idle") {
    CHECK(sm.state() == "/Root/Active/Idle");
    sm.clear();

    sm.dispatch<E_InternalAtComposite>();
    task.resume();

    CHECK(sm.counters.effect_internal == 1);
    CHECK(sm.state() == "/Root/Active/Idle");
  }
}

TEST_CASE("Internal transition inheritance - from Processing (with activity)") {
  // With native coroutines, the activity runs synchronously when entering Processing.
  struct Machine : Spy, HSM<inheritance_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Navigate to Processing state
  sm.dispatch<E_GoToProcessing>();
  task.resume();
  CHECK(sm.state() == "/Root/Active/Processing");

  // Activity should have already run synchronously
  CHECK(sm.counters.activity_started == 1);
  CHECK(sm.counters.activity_completed == 1);
  sm.clear();

  SUBCASE("Internal transition works after activity has completed") {
    // Query while in Processing with activity already done
    int result = -1;
    E_Query query;
    query.result = &result;
    sm.dispatch(query);
    task.resume();

    // Per UML semantics, internal transition at Active should be found
    // and executed even when we're in the Processing sub-state
    CHECK(result == 42);
    CHECK(sm.counters.effect_query == 1);

    // Should remain in Processing
    CHECK(sm.state() == "/Root/Active/Processing");
    CHECK(sm.counters.exit_Processing == 0);
    CHECK(sm.counters.entry_Processing == 0);
    CHECK(sm.counters.exit_Active == 0);
    CHECK(sm.counters.entry_Active == 0);
  }

  SUBCASE("Generic internal transition works from Processing") {
    sm.dispatch<E_InternalAtComposite>();
    task.resume();

    CHECK(sm.counters.effect_internal == 1);
    CHECK(sm.state() == "/Root/Active/Processing");
  }
}

TEST_CASE("Internal transition inheritance - multiple levels") {
  struct Machine : Spy, HSM<inheritance_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Test that internal transition is found by walking up the hierarchy
  // from the current leaf state (Idle or Processing) to the composite
  // state (Active) where the transition is defined.

  SUBCASE("Transition search walks up hierarchy correctly") {
    CHECK(sm.state() == "/Root/Active/Idle");

    // The transition chain for (Idle, E_Query) should include:
    // 1. Transitions at Idle (none for E_Query)
    // 2. Transitions at Active (has internal transition for E_Query)
    int result = -1;
    E_Query query;
    query.result = &result;
    sm.dispatch(query);
    task.resume();

    CHECK(result == 42);
  }
}

TEST_CASE("Comparison: external transition at composite works from sub-states") {
  // This test verifies that external transitions (with target) at composite
  // level work correctly from sub-states. If this passes but internal
  // transitions fail, the bug is specific to internal transition handling.

  struct E_ToExternal : Event<make_kind(10, Kind::Event)> {};

  static constexpr auto model_with_external = define(
      "Root",
      initial(target("/Root/Active/Idle")),

      state("Active",
            entry(&Spy::on_entry_Active),
            exit(&Spy::on_exit_Active),
            initial(target("/Root/Active/Idle")),

            // External transition at composite level
            transition(on<E_ToExternal>(), target("/Root/Other"),
                       effect(&Spy::on_effect_internal)),

            state("Idle",
                  entry(&Spy::on_entry_Idle),
                  exit(&Spy::on_exit_Idle))),

      state("Other"));

  struct ExternalMachine : Spy, HSM<model_with_external, ExternalMachine> {};
  ExternalMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/Root/Active/Idle");
  sm.clear();

  sm.dispatch<E_ToExternal>();
  task.resume();

  // External transition from composite should work
  CHECK(sm.state() == "/Root/Other");
  CHECK(sm.counters.effect_internal == 1);
  CHECK(sm.counters.exit_Idle == 1);
  CHECK(sm.counters.exit_Active == 1);
}
