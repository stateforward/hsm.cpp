#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"
#include "hsm/kind.hpp"

using namespace hsm;

struct GoToComposite : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct GoToChild2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct ToComp2 : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};

// History events
struct Next : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct Leave : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};
struct BackDeep : hsm::Event<hsm::make_kind(12, hsm::Kind::Event)> {};
struct BackDefault : hsm::Event<hsm::make_kind(13, hsm::Kind::Event)> {};
struct BackShallow : hsm::Event<hsm::make_kind(14, hsm::Kind::Event)> {};

// Additional events for multi-composite and guarded history tests
struct NextA : hsm::Event<hsm::make_kind(16, hsm::Kind::Event)> {};
struct NextB : hsm::Event<hsm::make_kind(17, hsm::Kind::Event)> {};
struct LeaveA : hsm::Event<hsm::make_kind(18, hsm::Kind::Event)> {};
struct LeaveB : hsm::Event<hsm::make_kind(19, hsm::Kind::Event)> {};
struct BackDeepA : hsm::Event<hsm::make_kind(20, hsm::Kind::Event)> {};
struct BackShallowB : hsm::Event<hsm::make_kind(21, hsm::Kind::Event)> {};
struct EnterBDirect : hsm::Event<hsm::make_kind(22, hsm::Kind::Event)> {};
struct HistReturn : hsm::Event<hsm::make_kind(23, hsm::Kind::Event)> {};
// Event used for named history default tests
struct HistNamed : hsm::Event<hsm::make_kind(24, hsm::Kind::Event)> {};
// Events used for named shallow/deep history comparison tests
struct HistNamedShallow : hsm::Event<hsm::make_kind(25, hsm::Kind::Event)> {};
struct HistNamedDeep : hsm::Event<hsm::make_kind(26, hsm::Kind::Event)> {};

struct CompositeInstance {
  std::vector<std::string> execution_log;
  int effect_count{0};
  int entry_count{0};
  int exit_count{0};

  void log(const std::string& message) { execution_log.push_back(message); }

  void clear_log() {
    execution_log.clear();
    effect_count = 0;
    entry_count = 0;
    exit_count = 0;
  }
};

// Helper instance and behaviors for guarded history tests
struct HistGuardInstance {
  bool allow_history = false;
  std::vector<std::string> log;
};

inline bool hist_guard(Signal&, HistGuardInstance& inst, const EventBase&) {
  inst.log.push_back("guard_history");
  return inst.allow_history;
}

inline void hist_effect(Signal&, HistGuardInstance& inst, const EventBase&) {
  inst.log.push_back("effect_history");
}

// Behaviors
void entry_composite(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_composite");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void exit_composite(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("exit_composite");
  static_cast<CompositeInstance&>(i).exit_count++;
}

void entry_child1(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_child1");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void exit_child1(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("exit_child1");
  static_cast<CompositeInstance&>(i).exit_count++;
}

void entry_child2(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_child2");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void exit_child2(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("exit_child2");
  static_cast<CompositeInstance&>(i).exit_count++;
}

void entry_other(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_other");
  static_cast<CompositeInstance&>(i).entry_count++;
}

void effect_initial(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("effect_initial");
  static_cast<CompositeInstance&>(i).effect_count++;
}

void entry_outer(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_outer");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_inner(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_inner");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_deepest(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_deepest");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_deepest_alt(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_deepest_alt");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_inner_alt(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_inner_alt");
  static_cast<CompositeInstance&>(i).entry_count++;
}

void entry_start(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_start");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void exit_start(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("exit_start");
  static_cast<CompositeInstance&>(i).exit_count++;
}

void entry_comp1(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp1");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_comp1_child1(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp1_child1");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_comp1_child2(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp1_child2");
  static_cast<CompositeInstance&>(i).entry_count++;
}

void entry_comp2(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp2");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_comp2_child1(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp2_child1");
  static_cast<CompositeInstance&>(i).entry_count++;
}
void entry_comp2_child2(Signal&, CompositeInstance& i, const EventBase&) {
  static_cast<CompositeInstance&>(i).log("entry_comp2_child2");
  static_cast<CompositeInstance&>(i).entry_count++;
}

// --- Models (Global Scope) ---

constexpr auto basic_initial_model = define(
    "CompositeWithInitial",
    initial(target("/CompositeWithInitial/composite")),
    state("composite", entry(entry_composite), exit(exit_composite),
          initial(target("/CompositeWithInitial/composite/child1")),
          state("child1", entry(entry_child1), exit(exit_child1)),
          state("child2", entry(entry_child2), exit(exit_child2))),
    state("other", entry(entry_other)));

constexpr auto effect_initial_model = define(
    "CompositeInitialWithEffect",
    initial(target("/CompositeInitialWithEffect/composite")),
    state("composite", entry(entry_composite),
          initial(target("/CompositeInitialWithEffect/composite/child1"),
                  effect(effect_initial)),
          state("child1", entry(entry_child1)),
          state("child2", entry(entry_child2))));

constexpr auto nested_initial_model = define(
    "NestedComposites", initial(target("/NestedComposites/outer")),
    state("outer", entry(entry_outer),
          initial(target("/NestedComposites/outer/inner")),
          state("inner", entry(entry_inner),
                initial(target("/NestedComposites/outer/inner/deepest")),
                state("deepest", entry(entry_deepest)),
                state("deepest_alt", entry(entry_deepest_alt))),
          state("inner_alt", entry(entry_inner_alt))));

constexpr auto transition_to_comp_model = define(
    "TransitionToComposite",
    initial(target("/TransitionToComposite/start")),
    state("start", entry(entry_start), exit(exit_start),
          transition(on<GoToComposite>(),
                     target("/TransitionToComposite/composite"))),
    state("composite", entry(entry_composite),
          initial(target("/TransitionToComposite/composite/child1")),
          state("child1", entry(entry_child1)),
          state("child2", entry(entry_child2))));

constexpr auto direct_nested_model = define(
    "DirectToNested", initial(target("/DirectToNested/start")),
    state("start", entry(entry_start), exit(exit_start),
          transition(on<GoToChild2>(),
                     target("/DirectToNested/composite/child2"))),
    state(
        "composite", entry(entry_composite),
        initial(target("/DirectToNested/composite/child1")),  // This should
                                                              // be bypassed
        state("child1", entry(entry_child1)),
        state("child2", entry(entry_child2))));

constexpr auto multi_comp_model = define(
    "MultipleComposites", initial(target("/MultipleComposites/comp1")),
    state("comp1", entry(entry_comp1),
          initial(target("/MultipleComposites/comp1/comp1_child1")),
          state("comp1_child1", entry(entry_comp1_child1)),
          state("comp1_child2", entry(entry_comp1_child2)),
          transition(on<ToComp2>(), target("/MultipleComposites/comp2"))),
    state("comp2", entry(entry_comp2),
          initial(target(
              "/MultipleComposites/comp2/comp2_child2")),  // Different
                                                           // initial child
          state("comp2_child1", entry(entry_comp2_child1)),
          state("comp2_child2", entry(entry_comp2_child2))));

constexpr auto abs_initial_model = define(
    "CompositeAbsoluteInitial",
    initial(target("/CompositeAbsoluteInitial/composite")),
    state("composite", entry(entry_composite),
          initial(target("/CompositeAbsoluteInitial/composite/child2")),
          state("child1", entry(entry_child1)),
          state("child2", entry(entry_child2))));

constexpr auto deep_history_model = define(
    "DeepHistoryMachine", 
    initial(target("/DeepHistoryMachine/P")),
    state("P", initial(target("/DeepHistoryMachine/P/S1")),
          state("S1", transition(on<Next>(), target("/DeepHistoryMachine/P/S2"))), state("S2"),
          transition(on<Leave>(), target("/DeepHistoryMachine/Outside"))),
    state("Outside",
          transition(on<BackDeep>(),
                     target(deep_history("/DeepHistoryMachine/P"))),
          transition(on<BackDefault>(), target("/DeepHistoryMachine/P"))  // Should go to S1
          ));

constexpr auto shallow_history_model = define(
    "ShallowHistoryMachine",
    initial(target("/ShallowHistoryMachine/P")),
    state("P", initial(target("/ShallowHistoryMachine/P/S1")),
          state("S1", initial(target("/ShallowHistoryMachine/P/S1/S1a")),
                state("S1a", transition(on<Next>(), target("/ShallowHistoryMachine/P/S1/S1b"))),
                state("S1b")),
          state("S2"), transition(on<Leave>(), target("/ShallowHistoryMachine/Outside"))),
    state("Outside",
          transition(on<BackShallow>(),
                     target(shallow_history("/ShallowHistoryMachine/P"))),
          transition(on<BackDeep>(),
                     target(deep_history("/ShallowHistoryMachine/P")))));

constexpr auto multi_history_model = define(
    "MultiHistoryMachine",
    initial(target("/MultiHistoryMachine/A")),
    state("A",
          initial(target("/MultiHistoryMachine/A/A1")),
          state("A1",
              initial(target("/MultiHistoryMachine/A/A1/A1a")),
              state("A1a",
                    transition(on<NextA>(),
                                target("/MultiHistoryMachine/A/A1/A1b"))),
              state("A1b")),
          transition(on<LeaveA>(), target("/MultiHistoryMachine/Outside"))),
    state("B",
          initial(target("/MultiHistoryMachine/B/B1")),
          state("B1",
              initial(target("/MultiHistoryMachine/B/B1/B1a")),
              state("B1a",
                    transition(on<NextB>(),
                                target("/MultiHistoryMachine/B/B1/B1b"))),
              state("B1b")),
          transition(on<LeaveB>(), target("/MultiHistoryMachine/Outside"))),
    state("Outside",
          transition(on<BackDeepA>(),
                     target(deep_history("/MultiHistoryMachine/A"))),
          transition(on<BackShallowB>(),
                     target(shallow_history("/MultiHistoryMachine/B"))),
          transition(on<EnterBDirect>(),
                     target("/MultiHistoryMachine/B"))));

constexpr auto guarded_history_model = define(
    "GuardedHistoryMachine",
    initial(target("/GuardedHistoryMachine/Parent")),
    state("Parent",
          initial(target("/GuardedHistoryMachine/Parent/Inner1")),
          state("Inner1",
              transition(on<Next>(),
                          target("/GuardedHistoryMachine/Parent/Inner2"))),
          state("Inner2",
              transition(on<Leave>(),
                          target("/GuardedHistoryMachine/Outside")))),
    state("Outside",
          // First try deep history if the guard allows it
          transition(on<HistReturn>(), guard(hist_guard),
                     effect(hist_effect),
                     target(deep_history("/GuardedHistoryMachine/Parent"))),
          // Fallback when guard rejects history
          transition(on<HistReturn>(),
                     target("/GuardedHistoryMachine/Fallback"))),
    state("Fallback"));

constexpr auto named_history_default_model = define(
    "NamedHistoryDefaultMachine",
    initial(target("/NamedHistoryDefaultMachine/Parent")),
    state("Parent",
          // Default: when no prior deep history for Parent exists, go to
          // Inner2 and run hist_effect once.
          deep_history("hist",
                       target("/NamedHistoryDefaultMachine/Parent/Inner2"),
                       effect(hist_effect)),
          initial(target("/NamedHistoryDefaultMachine/Parent/Inner1")),
          state("Inner1",
              transition(on<Next>(),
                          target("/NamedHistoryDefaultMachine/Parent/Inner2"))),
          state("Inner2"),
          transition(on<Leave>(),
                     target("/NamedHistoryDefaultMachine/Outside"))),
    state("Outside",
          transition(on<HistNamed>(),
                     target("/NamedHistoryDefaultMachine/Parent/hist"))));

constexpr auto named_shallow_deep_model = define(
    "NamedShallowDeepMachine",
    initial(target("/NamedShallowDeepMachine/Parent")),
    state("Parent",
          shallow_history("sh",
                          target("/NamedShallowDeepMachine/Parent/S1")),
          deep_history("dh",
                       target("/NamedShallowDeepMachine/Parent/S1")),
          initial(target("/NamedShallowDeepMachine/Parent/S1")),
          state("S1",
              initial(target("/NamedShallowDeepMachine/Parent/S1/S1a")),
              state("S1a",
                    transition(on<Next>(),
                                target("/NamedShallowDeepMachine/Parent/S1/S1b"))),
              state("S1b")),
          transition(on<Leave>(),
                     target("/NamedShallowDeepMachine/Outside"))),
    state("Outside",
          transition(on<HistNamedShallow>(),
                     target("/NamedShallowDeepMachine/Parent/sh")),
          transition(on<HistNamedDeep>(),
                     target("/NamedShallowDeepMachine/Parent/dh"))));


TEST_CASE("Composite States with Initial Pseudostates") {
  SUBCASE("Basic Composite State with Initial") {
    struct Machine : CompositeInstance, HSM<basic_initial_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    // The machine should enter composite state and then automatically enter
    // child1
    CHECK(sm.state() == "/CompositeWithInitial/composite/child1");
    CHECK(sm.execution_log.size() == 2);
    CHECK(sm.execution_log[0] == "entry_composite");
    CHECK(sm.execution_log[1] == "entry_child1");
    CHECK(sm.entry_count == 2);
  }

  SUBCASE("Composite State with Initial and Effect") {
    struct Machine : CompositeInstance, HSM<effect_initial_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    // Should execute: entry_composite, effect_initial, entry_child1
    CHECK(sm.state() == "/CompositeInitialWithEffect/composite/child1");
    CHECK(sm.execution_log.size() == 3);
    CHECK(sm.execution_log[0] == "entry_composite");
    CHECK(sm.execution_log[1] == "effect_initial");
    CHECK(sm.execution_log[2] == "entry_child1");
    CHECK(sm.entry_count == 2);
    CHECK(sm.effect_count == 1);
  }

  SUBCASE("Nested Composite States with Initial") {
    struct Machine : CompositeInstance, HSM<nested_initial_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    // Should cascade through all initial transitions
    CHECK(sm.state() == "/NestedComposites/outer/inner/deepest");
    CHECK(sm.execution_log.size() == 3);
    CHECK(sm.execution_log[0] == "entry_outer");
    CHECK(sm.execution_log[1] == "entry_inner");
    CHECK(sm.execution_log[2] == "entry_deepest");
    CHECK(sm.entry_count == 3);
  }

  SUBCASE("Transition to Composite State Triggers Initial") {
    struct Machine : CompositeInstance, HSM<transition_to_comp_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/TransitionToComposite/start");
    sm.clear_log();
    sm.dispatch<GoToComposite>();
    task.resume();
    // Transition to composite state
    sm.dispatch<GoToComposite>();
    task.resume();

    // Should exit start, enter composite, then enter child1 via initial
    CHECK(sm.state() == "/TransitionToComposite/composite/child1");

    // hsm should correctly call exit_start
    CHECK(sm.execution_log.size() == 3);
    CHECK(sm.execution_log[0] == "exit_start");
    CHECK(sm.execution_log[1] == "entry_composite");
    CHECK(sm.execution_log[2] == "entry_child1");
  }

  SUBCASE("Direct Transition to Nested State Bypasses Initial") {
    struct Machine : CompositeInstance, HSM<direct_nested_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/DirectToNested/start");
    sm.clear_log();

    // Direct transition to child2
    sm.dispatch<GoToChild2>();
    task.resume();

    // Should go directly to child2, bypassing the initial transition
    CHECK(sm.state() == "/DirectToNested/composite/child2");

    // hsm should call exit_start
    CHECK(sm.execution_log.size() == 3);
    CHECK(sm.execution_log[0] == "exit_start");
    CHECK(sm.execution_log[1] == "entry_composite");
    CHECK(sm.execution_log[2] == "entry_child2");

    // Verify that child1 was NOT entered (initial was bypassed)
    for (const auto& log : sm.execution_log) {
      CHECK(log != "entry_child1");
    }
  }

  SUBCASE("Multiple Composite States with Different Initials") {
    struct Machine : CompositeInstance, HSM<multi_comp_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // First composite should enter child1
    CHECK(sm.state() == "/MultipleComposites/comp1/comp1_child1");
    sm.clear_log();

    // Transition to second composite
    sm.dispatch<ToComp2>();
    task.resume();

    // Second composite should enter child2 (its initial)
    CHECK(sm.state() == "/MultipleComposites/comp2/comp2_child2");
    // Last log should be entry of child2
    CHECK(sm.execution_log.back() == "entry_comp2_child2");
  }

  SUBCASE("Composite State Initial with Absolute Path") {
    struct Machine : CompositeInstance, HSM<abs_initial_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    // Should use absolute path to enter child2
    CHECK(sm.state() == "/CompositeAbsoluteInitial/composite/child2");
    CHECK(sm.execution_log.size() == 2);
    CHECK(sm.execution_log[0] == "entry_composite");
    CHECK(sm.execution_log[1] == "entry_child2");
  }
}

TEST_CASE("Deep History Restoration") {
  struct Machine : HSM<deep_history_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/DeepHistoryMachine/P/S1");

  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/DeepHistoryMachine/P/S2");

  sm.dispatch<Leave>();
  task.resume();
  CHECK(sm.state() == "/DeepHistoryMachine/Outside");

  // Test Deep History
  sm.dispatch<BackDeep>();
  task.resume();
  CHECK(sm.state() == "/DeepHistoryMachine/P/S2");

  // Reset and test default entry for contrast
  sm.dispatch<Leave>();
  task.resume();
  CHECK(sm.state() == "/DeepHistoryMachine/Outside");

  sm.dispatch<BackDefault>();
  task.resume();
  CHECK(sm.state() == "/DeepHistoryMachine/P/S1");
}

TEST_CASE("Shallow History Restoration") {
  struct Machine : HSM<shallow_history_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/ShallowHistoryMachine/P/S1/S1a");

  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/P/S1/S1b");

  sm.dispatch<Leave>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/Outside");

  // Test Shallow History
  // Should restore S1, then init to S1a
  sm.dispatch<BackShallow>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/P/S1/S1a");

  // Verify Deep History behaviour for contrast (should go to S1b)
  // First allow it to go to S1b again
  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/P/S1/S1b");

  sm.dispatch<Leave>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/Outside");

  sm.dispatch<BackDeep>();
  task.resume();
  CHECK(sm.state() == "/ShallowHistoryMachine/P/S1/S1b");
}

TEST_CASE("Multi-composite history independence") {
  SUBCASE("Deep and shallow history are isolated per composite") {
    struct Machine : HSM<multi_history_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Start in A composite initial chain
    CHECK(sm.state() == "/MultiHistoryMachine/A/A1/A1a");

    // Move A to a deeper leaf and record history
    sm.dispatch<NextA>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/A/A1/A1b");

    sm.dispatch<LeaveA>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/Outside");

    // Enter B directly and move to its deeper leaf
    sm.dispatch<EnterBDirect>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/B/B1/B1a");

    sm.dispatch<NextB>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/B/B1/B1b");

    sm.dispatch<LeaveB>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/Outside");

    // Deep history for A should restore its own last active leaf
    sm.dispatch<BackDeepA>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/A/A1/A1b");

    // Leaving A again and using shallow history for B should still
    // restore B's direct child and follow its initial chain
    sm.dispatch<LeaveA>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/Outside");

    sm.dispatch<BackShallowB>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/B/B1/B1a");
  }

  SUBCASE("Shallow history falls back to initial when no prior history") {
    struct Machine : HSM<multi_history_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // From A's composite, leave to Outside without ever visiting B
    CHECK(sm.state() == "/MultiHistoryMachine/A/A1/A1a");
    sm.dispatch<LeaveA>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/Outside");

    // Shallow history for B with no prior record should behave like initial
    sm.dispatch<BackShallowB>();
    task.resume();
    CHECK(sm.state() == "/MultiHistoryMachine/B/B1/B1a");
  }
}

TEST_CASE("History transitions with guards and effects") {
  SUBCASE("Guard passes -> history target and effect run") {
    struct Machine : HistGuardInstance,
                     HSM<guarded_history_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Drive Parent to Inner2 and then leave to Outside
    CHECK(sm.state() == "/GuardedHistoryMachine/Parent/Inner1");
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/GuardedHistoryMachine/Parent/Inner2");
    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/GuardedHistoryMachine/Outside");

    sm.allow_history = true;
    sm.log.clear();

    sm.dispatch<HistReturn>();
    task.resume();

    // Deep history should be taken and effect should run
    CHECK(sm.state() == "/GuardedHistoryMachine/Parent/Inner2");
    REQUIRE(sm.log.size() >= 2);
    CHECK(sm.log[0] == "guard_history");
    CHECK(sm.log[1] == "effect_history");
  }

  SUBCASE("Guard fails -> fallback transition, no effect") {
    struct Machine : HistGuardInstance,
                     HSM<guarded_history_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Drive Parent to Inner2 and then leave to Outside
    CHECK(sm.state() == "/GuardedHistoryMachine/Parent/Inner1");
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/GuardedHistoryMachine/Parent/Inner2");
    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/GuardedHistoryMachine/Outside");

    sm.allow_history = false;
    sm.log.clear();

    sm.dispatch<HistReturn>();
    task.resume();

    // When the guard fails, the history transition is skipped and the
    // fallback transition is taken instead. The guard is still evaluated
    // (logged), but the effect is not run.
    CHECK(sm.state() == "/GuardedHistoryMachine/Fallback");
    REQUIRE(sm.log.size() >= 1);
    CHECK(sm.log[0] == "guard_history");
  }
}

TEST_CASE("Named history pseudostate default behavior") {
  SUBCASE("Default used only when there is no prior history") {
    struct Machine : HistGuardInstance,
                     HSM<named_history_default_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Initial chain: Parent -> Inner1
    CHECK(sm.state() == "/NamedHistoryDefaultMachine/Parent/Inner1");

    // Leave Parent without ever recording history for it. At this point,
    // there is no last_active_leaf_ entry for Parent.
    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/NamedHistoryDefaultMachine/Outside");

    sm.log.clear();

    // First history return: no prior history -> use default deep history
    // for Parent, going directly to Inner2 and running hist_effect once.
    sm.dispatch<HistNamed>();
    task.resume();
    CHECK(sm.state() == "/NamedHistoryDefaultMachine/Parent/Inner2");
    REQUIRE(sm.log.size() >= 1);
    CHECK(sm.log[0] == "effect_history");

    // Leave again so that Parent's deep history now records Inner2 as the
    // last active leaf.
    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/NamedHistoryDefaultMachine/Outside");

    sm.log.clear();

    // Second history return: a prior deep history leaf exists, so the
    // named history behaves like standard deep history. The default is
    // not consulted and its effect is not run again.
    sm.dispatch<HistNamed>();
    task.resume();
    CHECK(sm.state() == "/NamedHistoryDefaultMachine/Parent/Inner2");
    CHECK(sm.log.empty());
  }
}

TEST_CASE("Named shallow vs deep history pseudostates") {
  SUBCASE("Shallow restores direct child initial; deep restores leaf") {
    struct Machine : HSM<named_shallow_deep_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Initial path: Parent -> S1 -> S1a
    CHECK(sm.state() == "/NamedShallowDeepMachine/Parent/S1/S1a");

    // Move to deeper leaf S1b so history records it.
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Parent/S1/S1b");

    // Leave Parent to Outside.
    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Outside");

    // Shallow history via named pseudostate: should restore S1, then
    // follow S1's initial to S1a (not S1b).
    sm.dispatch<HistNamedShallow>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Parent/S1/S1a");

    // Move to S1b again and record deep history.
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Parent/S1/S1b");

    sm.dispatch<Leave>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Outside");

    // Deep history via named pseudostate: should restore full leaf S1b.
    sm.dispatch<HistNamedDeep>();
    task.resume();
    CHECK(sm.state() == "/NamedShallowDeepMachine/Parent/S1/S1b");
  }
}
