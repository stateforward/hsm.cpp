#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

struct Finish : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct Success : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct Failure : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct Cancel : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct Restart : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct Continue : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};
struct WildcardEvent : hsm::Event<hsm::make_kind(7, hsm::Kind::Event)> {};
struct Complete : hsm::Event<hsm::make_kind(8, hsm::Kind::Event)> {};
struct Reset : hsm::Event<hsm::make_kind(9, hsm::Kind::Event)> {};
struct FinishInner : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct FinishOuter : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};
struct Step1 : hsm::Event<hsm::make_kind(12, hsm::Kind::Event)> {};
struct Step2 : hsm::Event<hsm::make_kind(13, hsm::Kind::Event)> {};
struct FinalEvent : hsm::Event<hsm::make_kind(14, hsm::Kind::Event)> {};
struct DeferredEvent : hsm::Event<hsm::make_kind(15, hsm::Kind::Event)> {};

struct FinalStatesInstance {
  std::vector<std::string> execution_log;
  bool final_reached{false};

  void log(const std::string& message) { execution_log.push_back(message); }

  void clear() {
    execution_log.clear();
    final_reached = false;
  }
};

// Action functions
void log_entry_start(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_start");
}
void log_entry_active(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_active");
}
void log_entry_container(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_container");
}
void log_entry_working(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_working");
}
void log_entry_reset(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_reset");
}
void log_entry_level1(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_level1");
}
void log_entry_level2(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_level2");
}
void log_entry_step1(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_step1");
}
void log_entry_step2(Signal&, FinalStatesInstance& i, const EventBase&) {
  static_cast<FinalStatesInstance&>(i).log("entry_step2");
}

// --- Models (Global Scope for CRTP) ---

constexpr auto simple_final_model = define(
    "SimpleFinal", initial(target("/SimpleFinal/start")),
    state("start", entry(log_entry_start),
          transition(on<Finish>(), target("/SimpleFinal/end"))),
    final("end"));

constexpr auto multiple_final_model = define(
    "MultipleFinal", initial(target("/MultipleFinal/active")),
    state("active", entry(log_entry_active),
          transition(on<Success>(), target("/MultipleFinal/success")),
          transition(on<Failure>(), target("/MultipleFinal/failure")),
          transition(on<Cancel>(), target("/MultipleFinal/cancelled"))),
    final("success"), final("failure"), final("cancelled"));

constexpr auto final_with_entry_model = define(
    "FinalWithEntry", initial(target("/FinalWithEntry/start")),
    state("start",
          transition(on<Finish>(), target("/FinalWithEntry/end"))),
    final("end"));

constexpr auto final_no_transitions_model = define(
    "FinalNoTransitions",
    initial(target("/FinalNoTransitions/start")),
    state("start", transition(on<Finish>(),
                              target("/FinalNoTransitions/end"))),
    final("end"));

constexpr auto nested_final_model = define(
    "NestedFinal",
    initial(target(
        "/NestedFinal/container/working")),  // Direct target to working
    state("container", entry(log_entry_container),
          // initial(target("working")), // Not using local initial for
          // simplicity/hsm preference
          state("working", entry(log_entry_working),
                transition(on<Complete>(),
                           target("/NestedFinal/container/done"))),
          final("done"),
          transition(on<Reset>(), target("/NestedFinal/reset"))),
    state("reset", entry(log_entry_reset)));

constexpr auto hierarchical_finals_model = define(
    "HierarchicalFinals",
    initial(target("/HierarchicalFinals/level1/level2")),
    state("level1", entry(log_entry_level1),
          // initial(target("level2")),
          state("level2", entry(log_entry_level2),
                transition(
                    on<FinishInner>(),
                    target("/HierarchicalFinals/level1/inner_done"))),
          final("inner_done"),
          transition(on<FinishOuter>(),
                     target("/HierarchicalFinals/outer_done"))),
    final("outer_done"));

constexpr auto immediate_final_model = define(
    "ImmediateFinal", initial(target("/ImmediateFinal/end")),
    final("end"));



constexpr auto rapid_final_model = define(
    "RapidFinal", initial(target("/RapidFinal/start")),
    state("start", entry(log_entry_start),
          transition(on<Step1>(), target("/RapidFinal/step1"))),
    state("step1", entry(log_entry_step1),
          transition(on<Step2>(), target("/RapidFinal/step2"))),
    state("step2", entry(log_entry_step2),
          transition(on<FinalEvent>(), target("/RapidFinal/end"))),
    final("end"));

TEST_CASE("Final States - Basic Functionality") {
  SUBCASE("Simple Final State") {
    struct Machine : FinalStatesInstance, HSM<simple_final_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/SimpleFinal/start");
    CHECK_FALSE(sm.final_reached);

    // Transition to final state
    sm.dispatch<Finish>();
    task.resume();

    CHECK(sm.state() == "/SimpleFinal/end");

    // Check execution order
    REQUIRE(sm.execution_log.size() >= 1);
    CHECK(sm.execution_log[0] == "entry_start");
  }

  SUBCASE("Final State with Multiple Paths") {
    struct Machine : FinalStatesInstance, HSM<multiple_final_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/MultipleFinal/active");

    SUBCASE("Success Path") {
      sm.dispatch<Success>();
      task.resume();
      CHECK(sm.state() == "/MultipleFinal/success");
    }

    SUBCASE("Failure Path") {
      sm.dispatch<Failure>();
      task.resume();
      CHECK(sm.state() == "/MultipleFinal/failure");
    }

    SUBCASE("Cancel Path") {
      sm.dispatch<Cancel>();
      task.resume();
      CHECK(sm.state() == "/MultipleFinal/cancelled");
    }
  }
}

TEST_CASE("Final States - Semantic Restrictions") {
  SUBCASE("Final State Cannot Have Entry Actions") {
    // In hsm, final() does not accept entry actions, so we can't even compile
    // if we tried. We just verify basic behavior.
    struct Machine : FinalStatesInstance, HSM<final_with_entry_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    sm.dispatch<Finish>();
    task.resume();
    CHECK(sm.state() == "/FinalWithEntry/end");

    bool found_final_entry = false;
    for (const auto& log : sm.execution_log) {
      if (log == "entry_end") {
        found_final_entry = true;
        break;
      }
    }
    CHECK_FALSE(found_final_entry);
  }

  // Skipping "Cannot Have Exit Actions" and "Cannot Have Outgoing Transitions"
  // as they are enforced by API

  SUBCASE("Final State Cannot Have Outgoing Transitions (Runtime Check)") {
    // Even if we dispatch events, it should stay in final state
    struct Machine : FinalStatesInstance, HSM<final_no_transitions_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    sm.dispatch<Finish>();
    task.resume();
    CHECK(sm.state() == "/FinalNoTransitions/end");

    // Further dispatches of events that this model knows about must be
    // ignored once we are in the final state. Re-dispatching Finish
    // should therefore leave the machine in the same final state.
    sm.dispatch<Finish>();
    task.resume();
    CHECK(sm.state() == "/FinalNoTransitions/end");

    sm.dispatch<Finish>();
    task.resume();
    CHECK(sm.state() == "/FinalNoTransitions/end");
  }
}

TEST_CASE("Final States - Hierarchical Scenarios") {
  SUBCASE("Final State in Nested State") {
    struct Machine : FinalStatesInstance, HSM<nested_final_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/NestedFinal/container/working");
    // Complete the nested work
    sm.dispatch<Complete>();
    task.resume();
    CHECK(sm.state() == "/NestedFinal/container/done");

    // Parent state should still be able to handle events
    sm.dispatch<Reset>();
    task.resume();
    CHECK(sm.state() == "/NestedFinal/reset");

    // Check execution order
    REQUIRE(sm.execution_log.size() >= 3);
    CHECK(sm.execution_log[0] == "entry_container");
    CHECK(sm.execution_log[1] == "entry_working");
    CHECK(sm.execution_log[2] == "entry_reset");
  }

  SUBCASE("Multiple Final States in Hierarchy") {
    struct Machine : FinalStatesInstance, HSM<hierarchical_finals_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/HierarchicalFinals/level1/level2");

    SUBCASE("Inner Final First") {
      // Finish inner state first
      sm.dispatch<FinishInner>();
      task.resume();
      CHECK(sm.state() == "/HierarchicalFinals/level1/inner_done");

      // Then finish outer state
      sm.dispatch<FinishOuter>();
      task.resume();
      CHECK(sm.state() == "/HierarchicalFinals/outer_done");
    }

    SUBCASE("Outer Final Direct") {
      // Finish outer state directly (should exit inner state)
      sm.dispatch<FinishOuter>();
      task.resume();
      CHECK(sm.state() == "/HierarchicalFinals/outer_done");
    }
  }
}

TEST_CASE("Final States - Error Conditions and Edge Cases") {
  SUBCASE("Final State as Initial Target") {
    struct Machine : FinalStatesInstance, HSM<immediate_final_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    // Should go directly to final state
    CHECK(sm.state() == "/ImmediateFinal/end");
  }

  // NOTE: Final states still cannot declare defer() in the DSL
  // (final() only takes a name), so events sent after reaching a
  // final state are simply ignored if they are part of the model,
  // and attempting to dispatch events that the model does not know
  // about is now a compile-time error.

  SUBCASE("Rapid Transitions to Final State") {
    struct Machine : FinalStatesInstance, HSM<rapid_final_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/RapidFinal/start");

    // Rapid succession of events leading to final state
    sm.dispatch<Step1>();
    task.resume();
    CHECK(sm.state() == "/RapidFinal/step1");

    sm.dispatch<Step2>();
    task.resume();
    CHECK(sm.state() == "/RapidFinal/step2");

    sm.dispatch<FinalEvent>();
    task.resume();
    CHECK(sm.state() == "/RapidFinal/end");

    // Verify all entry actions were called
    REQUIRE(sm.execution_log.size() >= 3);
    CHECK(sm.execution_log[0] == "entry_start");
    CHECK(sm.execution_log[1] == "entry_step1");
    CHECK(sm.execution_log[2] == "entry_step2");
  }
}
