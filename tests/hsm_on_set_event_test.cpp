#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

using Signal = hsm::Signal;

struct AttrInstance {
  int value_triggers{0};
  int flag_triggers{0};
  int other_triggers{0};
};

// Behaviors for attribute-change tests
static void on_value(Signal&, AttrInstance &inst, const hsm::AnyEvent &) {
  ++inst.value_triggers;
}

static void on_flag(Signal&, AttrInstance &inst, const hsm::AnyEvent &) {
  ++inst.flag_triggers;
}

// Model using when("name") on deduced attributes.
constexpr auto when_deduced_model = define(
    "when_deduced_machine",
    attribute("value", 0),            // int
    attribute("flag", false),         // bool
    attribute("unused", 123),         // has no when(), should not trigger anything
    initial(target("/when_deduced_machine/idle")),
    state("idle",
          transition(when("value"),
                     target("/when_deduced_machine/idle"),
                     effect(on_value)),
          transition(when("flag"),
                     target("/when_deduced_machine/idle"),
                     effect(on_flag))));

// Model with multiple independent attributes each with their own when().
constexpr auto multi_attr_when_model = define(
    "multi_attr_machine",
    attribute("a", 0),
    attribute("b", 0),
    initial(target("/multi_attr_machine/idle")),
    state("idle",
          transition(when("a"),
                     target("/multi_attr_machine/idle"),
                     effect(on_value)),
          transition(when("b"),
                     target("/multi_attr_machine/idle"),
                     effect(on_flag))));

struct WhenInstance {
  int triggered{0};
};

auto on_trigger = [](Signal&, WhenInstance& inst, const EventBase&) {
  ++inst.triggered;
};

// Model using attribute("value") and when("value") sugar (from timer_test originally)
constexpr auto timer_when_model = define(
    "when_machine",
    attribute<int>("value", 0),
    initial(target("/when_machine/waiting")),
    state("waiting",
          transition(when("value"),
                     target("/when_machine/done"),
                     effect(on_trigger))),
    state("done"));

// Model that stays in the same state on attribute change, used to
// verify that repeated changes fire effects only when the value
// actually changes.
constexpr auto timer_when_immediate_model = define(
    "when_immediate",
    attribute<int>("value", 0),
    initial(target("/when_immediate/idle")),
    state("idle",
          transition(when("value"),
                     target("/when_immediate/idle"),
                     effect(on_trigger))));


// --- Tests ---

TEST_CASE("Attributes - when(\"name\") fires on change for deduced attributes") {
  struct Machine : AttrInstance, HSM<when_deduced_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/when_deduced_machine/idle");
  CHECK(sm.value_triggers == 0);
  CHECK(sm.flag_triggers == 0);
  CHECK(sm.other_triggers == 0);

  // Changing value should trigger once.
  sm.set<"value">(1);
  task.resume();
  CHECK(sm.value_triggers == 1);
  CHECK(sm.flag_triggers == 0);
  CHECK(sm.other_triggers == 0);
  CHECK(sm.state() == "/when_deduced_machine/idle");

  // Setting the same value again should be a no-op.
  sm.set<"value">(1);
  task.resume();
  CHECK(sm.value_triggers == 1);

  // Changing to a different value should trigger again.
  sm.set<"value">(2);
  task.resume();
  CHECK(sm.value_triggers == 2);

  // Changing the flag attribute should trigger the flag effect.
  sm.set<"flag">(true);
  task.resume();
  CHECK(sm.flag_triggers == 1);

  // Setting the same flag value again is a no-op.
  sm.set<"flag">(true);
  task.resume();
  CHECK(sm.flag_triggers == 1);

  // Changing an attribute with no when("unused") transition should not
  // trigger any effects.
  sm.set<"unused">(999);
  task.resume();
  CHECK(sm.value_triggers == 2);
  CHECK(sm.flag_triggers == 1);
  CHECK(sm.other_triggers == 0);
}

TEST_CASE("Attributes - independent when(\"a\") and when(\"b\") do not cross-trigger") {
  struct Machine : AttrInstance, HSM<multi_attr_when_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/multi_attr_machine/idle");
  CHECK(sm.value_triggers == 0);
  CHECK(sm.flag_triggers == 0);

  // Change attribute a only
  sm.set<"a">(1);
  task.resume();
  CHECK(sm.value_triggers == 1);
  CHECK(sm.flag_triggers == 0);

  // Change attribute b only
  sm.set<"b">(5);
  task.resume();
  CHECK(sm.value_triggers == 1);
  CHECK(sm.flag_triggers == 1);

  // Re-setting with same values should not trigger
  sm.set<"a">(1);
  task.resume();
  sm.set<"b">(5);
  task.resume();
  CHECK(sm.value_triggers == 1);
  CHECK(sm.flag_triggers == 1);
}

TEST_CASE("Events - events<Name> maps attributes to ChangeEvent") {
  struct Machine : AttrInstance, HSM<when_deduced_model, Machine> {};
  using M = Machine;

  using ValueEvent = typename M::template events<"value">::type;
  using FlagEvent  = typename M::template events<"flag">::type;
  using UnusedEvent = typename M::template events<"unused">::type;
  (void) sizeof(UnusedEvent); // suppress unused-type warnings

  static_assert(M::template events<"value">::is_attribute);
  static_assert(!M::template events<"value">::is_operation);
  static_assert(!M::template events<"value">::is_plain_event);

  static_assert(hsm::is_kind(ValueEvent::kind, hsm::Kind::ChangeEvent));
  static_assert(M::template events<"value">::supported());

  static_assert(M::template events<"flag">::is_attribute);
  static_assert(hsm::is_kind(FlagEvent::kind, hsm::Kind::ChangeEvent));
  static_assert(M::template events<"flag">::supported());

  // Attribute with no when()/on_set() still maps to a ChangeEvent-kind type
  // but is not actually supported by the model's event table.
  static_assert(M::template events<"unused">::is_attribute);
  static_assert(!M::template events<"unused">::supported());
}

TEST_CASE("Attributes - when<MemberPtr>() drives transition on change") {
  struct Machine : WhenInstance, HSM<timer_when_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/when_machine/waiting");
  CHECK(sm.triggered == 0);

  sm.set<"value">(1);
  task.resume();

  CHECK(sm.triggered == 1);
  CHECK(sm.state() == "/when_machine/done");
}

TEST_CASE("Attributes - ChangeEvent does not fire on unchanged value") {
  struct Machine : WhenInstance,
                   HSM<timer_when_immediate_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/when_immediate/idle");
  CHECK(sm.triggered == 0);

  // First change should trigger once.
  sm.set<"value">(5);
  task.resume();
  CHECK(sm.triggered == 1);
  CHECK(sm.state() == "/when_immediate/idle");

  // Setting the same value again should be a no-op.
  sm.set<"value">(5);
  task.resume();
  CHECK(sm.triggered == 1);

  // Changing the value again should trigger a second time.
  sm.set<"value">(7);
  task.resume();
  CHECK(sm.triggered == 2);
}
