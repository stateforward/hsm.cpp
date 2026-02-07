#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

struct Evaluate : hsm::Event<hsm::make_kind(100, hsm::Kind::Event)> {};
struct Choose : hsm::Event<hsm::make_kind(101, hsm::Kind::Event)> {};
struct Test : hsm::Event<hsm::make_kind(102, hsm::Kind::Event)> {};
struct Go : hsm::Event<hsm::make_kind(103, hsm::Kind::Event)> {};
struct Decide : hsm::Event<hsm::make_kind(104, hsm::Kind::Event)> {};
struct Exit : hsm::Event<hsm::make_kind(105, hsm::Kind::Event)> {};
struct First : hsm::Event<hsm::make_kind(106, hsm::Kind::Event)> {};
struct Second : hsm::Event<hsm::make_kind(107, hsm::Kind::Event)> {};
struct Next : hsm::Event<hsm::make_kind(108, hsm::Kind::Event)> {};

struct ChoiceInstance {
  std::vector<std::string> execution_log;
  int value{0};
  bool condition_a{false};
  bool condition_b{false};

  void log(const std::string& message) { execution_log.push_back(message); }

  void clear() {
    execution_log.clear();
    value = 0;
    condition_a = false;
    condition_b = false;
  }
};

// Behaviors
void entry_start(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_start");
}
void entry_positive(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_positive");
}
void entry_negative(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_negative");
}

void entry_large(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_large");
}
void entry_small_positive(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_small_positive");
}
void entry_even(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_even");
}
void entry_other(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_other");
}

void entry_fallback(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_fallback");
}
void entry_default(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_default");
}

void entry_path_a(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_path_a");
}
void entry_path_b(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_path_b");
}

void effect_choice_to_a(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("effect_choice_to_a");
}
void effect_choice_to_b(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("effect_choice_to_b");
}
void effect_choice_to_default(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("effect_choice_to_default");
}

void entry_container(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_container");
}
void entry_outside(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_outside");
}

void entry_middle_a(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_middle_a");
}
void entry_middle_b(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_middle_b");
}
void entry_end_even(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_end_even");
}
void entry_end_odd(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_end_odd");
}

void entry_target(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_target");
}

void entry_positive_and_a(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_positive_and_a");
}
void entry_positive_and_b(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_positive_and_b");
}
void entry_just_positive(Signal&, ChoiceInstance& i, const EventBase&) {
  static_cast<ChoiceInstance&>(i).log("entry_just_positive");
}

// Guards
bool guard_value_positive(Signal&, ChoiceInstance& i, const EventBase&) {
  return static_cast<ChoiceInstance&>(i).value > 0;
}
bool guard_value_even(Signal&, ChoiceInstance& i, const EventBase&) {
  return (static_cast<ChoiceInstance&>(i).value % 2) == 0;
}
bool guard_value_greater_than_5(Signal&, ChoiceInstance& i, const EventBase&) {
  return static_cast<ChoiceInstance&>(i).value > 5;
}
bool guard_condition_a(Signal&, ChoiceInstance& i, const EventBase&) {
  return static_cast<ChoiceInstance&>(i).condition_a;
}
bool guard_condition_b(Signal&, ChoiceInstance& i, const EventBase&) {
  return static_cast<ChoiceInstance&>(i).condition_b;
}
bool guard_always_true(Signal&, ChoiceInstance&, const EventBase&) { return true; }
bool guard_always_false(Signal&, ChoiceInstance&, const EventBase&) { return false; }

// Complex guards
bool guard_positive_and_a(Signal&, ChoiceInstance& i, const EventBase&) {
  auto& inst = static_cast<ChoiceInstance&>(i);
  return inst.value > 0 && inst.condition_a;
}
bool guard_positive_and_b(Signal&, ChoiceInstance& i, const EventBase&) {
  auto& inst = static_cast<ChoiceInstance&>(i);
  return inst.value > 0 && inst.condition_b;
}

// --- Models (Global Scope) ---

constexpr auto simple_choice_model = define(
    "SimpleChoice", initial(target("/SimpleChoice/start")),
    state("start", entry(entry_start),
          transition(on<Evaluate>(), target("/SimpleChoice/choice"))),
    choice(
        "choice",
        transition(guard(guard_value_positive),
                   target("/SimpleChoice/positive")),
        transition(target("/SimpleChoice/negative"))  // Guardless fallback
        ),
    state("positive", entry(entry_positive)),
    state("negative", entry(entry_negative)));

constexpr auto multiple_guards_model = define(
    "MultipleGuards", initial(target("/MultipleGuards/start")),
    state("start", entry(entry_start),
          transition(on<Evaluate>(), target("/MultipleGuards/choice"))),
    choice(
        "choice",
        transition(guard(guard_value_greater_than_5),
                   target("/MultipleGuards/large")),
        transition(guard(guard_value_positive),
                   target("/MultipleGuards/small_positive")),
        transition(guard(guard_value_even), target("/MultipleGuards/even")),
        transition(target("/MultipleGuards/other"))  // Guardless fallback
        ),
    state("large", entry(entry_large)),
    state("small_positive", entry(entry_small_positive)),
    state("even", entry(entry_even)), state("other", entry(entry_other)));

constexpr auto guardless_fallback_model = define(
    "GuardlessFallback", initial(target("/GuardlessFallback/start")),
    state("start",
          transition(on<Choose>(), target("/GuardlessFallback/choice"))),
    choice("choice",
           transition(guard(guard_always_false),
                      target("/GuardlessFallback/never")),
           transition(guard(guard_always_false),
                      target("/GuardlessFallback/also_never")),
           transition(target("/GuardlessFallback/fallback"))),
    state("never"), state("also_never"),
    state("fallback", entry(entry_fallback)));

constexpr auto all_guards_fail_model = define(
    "AllGuardsFail", initial(target("/AllGuardsFail/start")),
    state("start", transition(on<Test>(), target("/AllGuardsFail/choice"))),
    choice("choice",
           transition(guard(guard_condition_a),
                      target("/AllGuardsFail/option_a")),
           transition(guard(guard_condition_b),
                      target("/AllGuardsFail/option_b")),
           transition(target("/AllGuardsFail/default"))),
    state("option_a"), state("option_b"),
    state("default", entry(entry_default)));

constexpr auto choice_effects_model = define(
    "ChoiceWithEffects", initial(target("/ChoiceWithEffects/start")),
    state("start",
          transition(on<Go>(), target("/ChoiceWithEffects/choice"))),
    choice("choice",
           transition(guard(guard_condition_a),
                      target("/ChoiceWithEffects/path_a"),
                      effect(effect_choice_to_a)),
           transition(guard(guard_condition_b),
                      target("/ChoiceWithEffects/path_b"),
                      effect(effect_choice_to_b)),
           transition(target("/ChoiceWithEffects/default"),
                      effect(effect_choice_to_default))),
    state("path_a", entry(entry_path_a)),
    state("path_b", entry(entry_path_b)),
    state("default", entry(entry_default)));

constexpr auto nested_choice_model = define(
    "NestedChoice", initial(target("/NestedChoice/container/start")),
    state("container", entry(entry_container),
          // initial(target("/NestedChoice/container/start")), // hsm:
          // initial transitions are properties of state or use global
          // initial Using direct target in global initial above
          state("start", entry(entry_start),
                transition(on<Decide>(),
                           target("/NestedChoice/container/choice"))),
          choice("choice",
                 transition(guard(guard_value_positive),
                            target("/NestedChoice/container/positive")),
                 transition(target("/NestedChoice/container/negative"))),
          state("positive", entry(entry_positive)),
          state("negative", entry(entry_negative)),
          transition(on<Exit>(), target("/NestedChoice/outside"))),
    state("outside", entry(entry_outside)));

constexpr auto sequential_choices_model = define(
    "SequentialChoices", initial(target("/SequentialChoices/start")),
    state("start",
          transition(on<First>(), target("/SequentialChoices/choice1"))),
    choice("choice1",
           transition(guard(guard_condition_a),
                      target("/SequentialChoices/middle_a")),
           transition(target("/SequentialChoices/middle_b"))),
    state("middle_a", entry(entry_middle_a),
          transition(on<Second>(), target("/SequentialChoices/choice2"))),
    state("middle_b", entry(entry_middle_b),
          transition(on<Second>(), target("/SequentialChoices/choice2"))),
    choice("choice2",
           transition(guard(guard_value_even),
                      target("/SequentialChoices/end_even")),
           transition(target("/SequentialChoices/end_odd"))),
    state("end_even", entry(entry_end_even)),
    state("end_odd", entry(entry_end_odd)));

constexpr auto initial_choice_model = define(
    "InitialChoice", initial(target("/InitialChoice/choice")),
    choice("choice", transition(target("/InitialChoice/target"))),
    state("target", entry(entry_target)));

constexpr auto rapid_choices_model = define(
    "RapidChoices", initial(target("/RapidChoices/start")),
    state("start", transition(on<Go>(), target("/RapidChoices/choice"))),
    choice("choice",
           transition(guard(guard_value_positive),
                      target("/RapidChoices/positive")),
           transition(target("/RapidChoices/negative"))),
    state("positive", entry(entry_positive),
          transition(on<Next>(), target("/RapidChoices/choice"))),
    state("negative", entry(entry_negative),
          transition(on<Next>(), target("/RapidChoices/choice"))));

constexpr auto complex_guards_model = define(
    "ComplexGuards", initial(target("/ComplexGuards/start")),
    state("start",
          transition(on<Evaluate>(), target("/ComplexGuards/choice"))),
    choice("choice",
           transition(guard(guard_positive_and_a),
                      target("/ComplexGuards/positive_and_a")),
           transition(guard(guard_positive_and_b),
                      target("/ComplexGuards/positive_and_b")),
           transition(guard(guard_value_positive),
                      target("/ComplexGuards/just_positive")),
           transition(target("/ComplexGuards/other"))),
    state("positive_and_a", entry(entry_positive_and_a)),
    state("positive_and_b", entry(entry_positive_and_b)),
    state("just_positive", entry(entry_just_positive)),
    state("other", entry(entry_other)));

TEST_CASE("Choice States - Basic Functionality") {
  SUBCASE("Simple Choice with Two Options") {
    struct Machine : ChoiceInstance, HSM<simple_choice_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    SUBCASE("Positive Path") {
      sm.value = 5;

      CHECK(sm.state() == "/SimpleChoice/start");

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/SimpleChoice/positive");

      REQUIRE(sm.execution_log.size() >= 2);
      CHECK(sm.execution_log[0] == "entry_start");
      CHECK(sm.execution_log[1] == "entry_positive");
    }

    SUBCASE("Negative Path (Fallback)") {
      sm.value = -3;

      CHECK(sm.state() == "/SimpleChoice/start");

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/SimpleChoice/negative");

      REQUIRE(sm.execution_log.size() >= 2);
      CHECK(sm.execution_log[0] == "entry_start");
      CHECK(sm.execution_log[1] == "entry_negative");
    }
  }

  SUBCASE("Choice with Multiple Guards") {
    struct Machine : ChoiceInstance, HSM<multiple_guards_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    SUBCASE("Large Value (First Guard)") {
      sm.value = 10;

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/MultipleGuards/large");
    }

    SUBCASE("Small Positive Value (Second Guard)") {
      sm.value = 3;

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/MultipleGuards/small_positive");
    }

    SUBCASE("Even Negative Value (Third Guard)") {
      sm.value = -4;

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/MultipleGuards/even");
    }

    SUBCASE("Odd Negative Value (Fallback)") {
      sm.value = -3;

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/MultipleGuards/other");
    }
  }
}

TEST_CASE("Choice States - Guardless Fallback Requirements") {
  SUBCASE("Choice Must Have Guardless Fallback") {
    struct Machine : ChoiceInstance, HSM<guardless_fallback_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    sm.dispatch<Choose>();
    task.resume();

    CHECK(sm.state() == "/GuardlessFallback/fallback");

    bool found_fallback_entry = false;
    for (const auto& log : sm.execution_log) {
      if (log == "entry_fallback") {
        found_fallback_entry = true;
        break;
      }
    }
    CHECK(found_fallback_entry);
  }

  SUBCASE("Fallback Used When All Guards Fail") {
    struct Machine : ChoiceInstance, HSM<all_guards_fail_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    sm.dispatch<Test>();
    task.resume();
    CHECK(sm.state() == "/AllGuardsFail/default");
  }
}

TEST_CASE("Choice States - Effects on Transitions") {
  SUBCASE("Choice Transitions with Effects") {
    struct Machine : ChoiceInstance, HSM<choice_effects_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    SUBCASE("Path A with Effect") {
      sm.condition_a = true;

      sm.dispatch<Go>();
      task.resume();
      CHECK(sm.state() == "/ChoiceWithEffects/path_a");

      bool found_effect = false;
      for (const auto& log : sm.execution_log) {
        if (log == "effect_choice_to_a") {
          found_effect = true;
          break;
        }
      }
      CHECK(found_effect);
    }

    SUBCASE("Default Path with Effect") {
      // Both conditions false

      sm.dispatch<Go>();
      task.resume();
      CHECK(sm.state() == "/ChoiceWithEffects/default");

      bool found_effect = false;
      for (const auto& log : sm.execution_log) {
        if (log == "effect_choice_to_default") {
          found_effect = true;
          break;
        }
      }
      CHECK(found_effect);
    }
  }
}

TEST_CASE("Choice States - Hierarchical Scenarios") {
  SUBCASE("Choice Inside Nested State") {
    struct Machine : ChoiceInstance, HSM<nested_choice_model, Machine> {};
    Machine sm;
    auto task = sm.start();
    sm.value = 7;

    CHECK(sm.state() == "/NestedChoice/container/start");

    sm.dispatch<Decide>();
    task.resume();
    CHECK(sm.state() == "/NestedChoice/container/positive");

    // Exit the container state
    sm.dispatch<Exit>();
    task.resume();
    CHECK(sm.state() == "/NestedChoice/outside");

    // Verify execution order
    REQUIRE(sm.execution_log.size() >= 4);
    CHECK(sm.execution_log[0] == "entry_container");
    CHECK(sm.execution_log[1] == "entry_start");
    CHECK(sm.execution_log[2] == "entry_positive");
    CHECK(sm.execution_log[3] == "entry_outside");
  }

  SUBCASE("Multiple Choice States in Sequence") {
    struct Machine : ChoiceInstance, HSM<sequential_choices_model, Machine> {};
    Machine sm;
    auto task = sm.start();
    sm.condition_a = true;
    sm.value = 6;

    sm.dispatch<First>();
    task.resume();
    CHECK(sm.state() == "/SequentialChoices/middle_a");

    sm.dispatch<Second>();
    task.resume();
    CHECK(sm.state() == "/SequentialChoices/end_even");

    REQUIRE(sm.execution_log.size() >= 2);
    CHECK(sm.execution_log[0] == "entry_middle_a");
    CHECK(sm.execution_log[1] == "entry_end_even");
  }
}

TEST_CASE("Choice States - Error Conditions and Edge Cases") {
  SUBCASE("Choice State Cannot Be Initial Target") {
    struct Machine : ChoiceInstance, HSM<initial_choice_model, Machine> {};
    Machine sm;
    [[maybe_unused]] auto task = sm.start();

    CHECK(sm.state() == "/InitialChoice/target");
  }

  SUBCASE("Rapid Choice Transitions") {
    struct Machine : ChoiceInstance, HSM<rapid_choices_model, Machine> {};
    Machine sm;
    auto task = sm.start();
    sm.value = 5;

    sm.dispatch<Go>();
    task.resume();
    CHECK(sm.state() == "/RapidChoices/positive");

    sm.value = -2;
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/RapidChoices/negative");

    sm.value = 3;
    sm.dispatch<Next>();
    task.resume();
    CHECK(sm.state() == "/RapidChoices/positive");
  }

  SUBCASE("Choice with Complex Guard Expressions") {
    struct Machine : ChoiceInstance, HSM<complex_guards_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    SUBCASE("Positive and A") {
      sm.value = 5;
      sm.condition_a = true;

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/ComplexGuards/positive_and_a");
    }

    SUBCASE("Just Positive") {
      sm.value = 3;
      // Neither condition_a nor condition_b is set

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/ComplexGuards/just_positive");
    }

    SUBCASE("Fallback") {
      sm.value = -1;  // Negative

      sm.dispatch<Evaluate>();
      task.resume();
      CHECK(sm.state() == "/ComplexGuards/other");
    }
  }
}

// Recursive Choice

constexpr auto choice_recursion_model = define(
    "RecursiveChoice", initial(target("/RecursiveChoice/start")),
    state("start", transition(on<Go>(), target("/RecursiveChoice/c1"))),
    choice("c1",
        transition(guard(guard_always_true), target("/RecursiveChoice/c2")),
        transition(target("/RecursiveChoice/fail"))
    ),
    choice("c2",
        transition(guard(guard_always_true), target("/RecursiveChoice/final")),
        transition(target("/RecursiveChoice/fail"))
    ),
    state("final"),
    state("fail")
);

TEST_CASE("Choice States - Recursive Chains") {
    struct Machine : ChoiceInstance, HSM<choice_recursion_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    sm.dispatch<Go>();
    task.resume();
    CHECK(sm.state() == "/RecursiveChoice/final");
}



