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



// --- Models ---

// Model with attributes using explicit T and type-deducing overloads.
constexpr auto attribute_deduce_model = define(
    "attr_deduce_machine",
    // Explicit type
    attribute<int>("explicit", 42),
    // Type-deducing attributes
    attribute("i_attr", 1),            // int
    attribute("d_attr", 1.5),          // double
    initial(target("/attr_deduce_machine/idle")),
    state("idle"));

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


// Model with attributes suitable for emplace-style construction.
constexpr auto emplace_attr_model = define(
    "emplace_attr_machine",
    attribute<std::string>("s"),
    attribute<std::vector<int>>("vec"),
    initial(target("/emplace_attr_machine/idle")),
    state("idle"));

// Model combining a when("value") attribute with additional complex
// attributes to exercise mixed set<> and emplace<> constructor usage.
constexpr auto set_emplace_model = define(
    "set_emplace_machine",
    attribute("value", 0),              // int with when("value")
    attribute<std::string>("s"),        // emplace-able
    attribute<std::vector<int>>("vec"), // emplace-able
    initial(target("/set_emplace_machine/idle")),
    state("idle",
          transition(when("value"),
                     target("/set_emplace_machine/idle"),
                     effect(on_value))));

// --- Tests ---

TEST_CASE("Attributes - type-deducing attribute(\"name\", default)") {
  struct Machine : AttrInstance, HSM<attribute_deduce_model, Machine> {};

  // Static type checks for get<"name">()
  using M = Machine;
  using ExplicitRef = decltype(std::declval<M&>().template get<"explicit">());
  using IRef       = decltype(std::declval<M&>().template get<"i_attr">());
  using DRef       = decltype(std::declval<M&>().template get<"d_attr">());

  static_assert(std::is_same_v<ExplicitRef, int&>, "explicit attribute should be int&");
  static_assert(std::is_same_v<IRef, int&>,       "i_attr should be int&");
  static_assert(std::is_same_v<DRef, double&>,    "d_attr should be double&");

  Machine sm;

  // Defaults from DSL
  CHECK(sm.get<"explicit">() == 42);
  CHECK(sm.get<"i_attr">() == 1);
  CHECK(sm.get<"d_attr">() == doctest::Approx(1.5));

  // Mutation via set<"name"> updates values (fire-and-forget since no when() listeners)
  (void)sm.set<"i_attr">(10);
  (void)sm.set<"d_attr">(2.25);

  CHECK(sm.get<"i_attr">() == 10);
  CHECK(sm.get<"d_attr">() == doctest::Approx(2.25));
}

TEST_CASE("Attributes - constructor set<\"name\"> overrides without firing when(\"name\")") {
  struct Machine : AttrInstance, HSM<when_deduced_model, Machine> {
    using Base = HSM<when_deduced_model, Machine>;
    using Base::Base; // inherit HSM constructors, including attribute-set ones
  };

  // 1) Plain constructor with attribute overrides only.
  Machine sm(hsm::set<"value">(42), hsm::set<"flag">(true));

  CHECK(sm.id() == "");
  CHECK(sm.get<"value">() == 42);
  CHECK(sm.value_triggers == 0);
  CHECK(sm.flag_triggers == 0);

  // Runtime mutation still drives change events.
  auto task = sm.start();  // Must start before mutations that trigger events
  sm.set<"value">(43);
  task.resume();
  CHECK(sm.get<"value">() == 43);
  CHECK(sm.value_triggers == 1);
  CHECK(sm.flag_triggers == 0);

  // 2) Constructor with id + attribute overrides.
  Machine sm_with_id("my_id", hsm::set<"value">(10));
  CHECK(sm_with_id.id() == "my_id");
  CHECK(sm_with_id.get<"value">() == 10);
  CHECK(sm_with_id.value_triggers == 0);
  CHECK(sm_with_id.flag_triggers == 0);

  // 3) Constructor with Signal + attribute overrides.
  Signal signal;
  Machine sm_with_signal(signal, hsm::set<"value">(7));
  CHECK(sm_with_signal.id() == "");
  CHECK(sm_with_signal.get<"value">() == 7);
  CHECK(sm_with_signal.value_triggers == 0);
  CHECK(sm_with_signal.flag_triggers == 0);

  // 4) Constructor with Signal + id + attribute overrides.
  Machine sm_with_signal_id(signal, "sid", hsm::set<"value">(9));
  CHECK(sm_with_signal_id.id() == "sid");
  CHECK(sm_with_signal_id.get<"value">() == 9);
  CHECK(sm_with_signal_id.value_triggers == 0);
  CHECK(sm_with_signal_id.flag_triggers == 0);
}

TEST_CASE("Attributes - constructor emplace<\"name\"> in-place initializes attributes") {
  struct Machine : AttrInstance, HSM<emplace_attr_model, Machine> {
    using Base = HSM<emplace_attr_model, Machine>;
    using Base::Base;
  };

  Machine sm(hsm::emplace<"s">("hello"),
             hsm::emplace<"vec">(3UL, 7));

  CHECK(sm.get<"s">() == std::string("hello"));
  const auto &vec = sm.get<"vec">();
  CHECK(vec.size() == 3);
  CHECK(vec[0] == 7);
  CHECK(vec[1] == 7);
  CHECK(vec[2] == 7);
}

TEST_CASE("Attributes - constructor mixing set<> and emplace<> is supported and silent") {
  struct Machine : AttrInstance, HSM<set_emplace_model, Machine> {
    using Base = HSM<set_emplace_model, Machine>;
    using Base::Base;
  };

  // Use both set<> and emplace<> in a single constructor. This must not
  // trigger when("value") at construction time.
  Machine sm(
      hsm::set<"value">(11),
      hsm::emplace<"s">("hi"),
      hsm::emplace<"vec">(2UL, 9));
  auto task = sm.start();

  CHECK(sm.state() == "/set_emplace_machine/idle");
  CHECK(sm.id() == "");

  CHECK(sm.get<"value">() == 11);
  CHECK(sm.get<"s">() == std::string("hi"));
  const auto &vec = sm.get<"vec">();
  CHECK(vec.size() == 2);
  CHECK(vec[0] == 9);
  CHECK(vec[1] == 9);

  // No constructor-time change events
  CHECK(sm.value_triggers == 0);
  CHECK(sm.flag_triggers == 0);
  CHECK(sm.other_triggers == 0);

  // Runtime set<> still drives when("value")
  sm.set<"value">(12);
  task.resume();
  CHECK(sm.get<"value">() == 12);
  CHECK(sm.value_triggers == 1);
}
