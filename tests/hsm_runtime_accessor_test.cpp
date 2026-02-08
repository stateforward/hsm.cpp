#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <any>
#include <string>

#include "hsm/hsm.hpp"

using namespace hsm;

using Signal = hsm::Signal;

struct RuntimeAttrInstance {
  int value_triggers{0};
};

static void on_value_change(Signal&, RuntimeAttrInstance &inst, const hsm::AnyEvent &) {
  ++inst.value_triggers;
}

// Model with attributes and a when() listener for change events.
constexpr auto runtime_model = define(
    "runtime_machine",
    attribute("value", 0),
    attribute("flag", false),
    attribute("unused", 123),
    initial(target("/runtime_machine/idle")),
    state("idle",
          transition(when("value"),
                     target("/runtime_machine/idle"),
                     effect(on_value_change))));

TEST_CASE("Runtime get - known attribute returns correct value") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  std::any result = sm.get("value");
  REQUIRE(result.has_value());
  const int* p = std::any_cast<int>(&result);
  REQUIRE(p != nullptr);
  CHECK(*p == 0);

  // Mutate via compile-time set, then read via runtime get
  (void)sm.set<"value">(42);
  result = sm.get("value");
  REQUIRE(result.has_value());
  p = std::any_cast<int>(&result);
  REQUIRE(p != nullptr);
  CHECK(*p == 42);
}

TEST_CASE("Runtime get - unknown attribute returns empty any") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  std::any result = sm.get("nonexistent");
  CHECK_FALSE(result.has_value());
}

TEST_CASE("Runtime set - known attribute with correct type updates value") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  result_t r = sm.set("unused", std::any(999));
  CHECK(r == Processed);
  CHECK(sm.get<"unused">() == 999);
}

TEST_CASE("Runtime set - known attribute with wrong type returns QueueFull") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  // "value" is int, pass a string
  result_t r = sm.set("value", std::any(std::string("wrong")));
  CHECK(r == QueueFull);
  // Value unchanged
  CHECK(sm.get<"value">() == 0);
}

TEST_CASE("Runtime set - unknown attribute returns QueueFull") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  result_t r = sm.set("nonexistent", std::any(42));
  CHECK(r == QueueFull);
}

TEST_CASE("Runtime set - same value returns Processed without triggering event") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  auto task = sm.start();
  // value is already 0, set to 0 again
  result_t r = sm.set("value", std::any(0));
  task.resume();
  CHECK(r == Processed);
  CHECK(sm.value_triggers == 0);
}

TEST_CASE("Runtime set - attribute with when() listener triggers transition") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  auto task = sm.start();

  result_t r = sm.set("value", std::any(10));
  task.resume();

  CHECK(r == Processed);
  CHECK(sm.get<"value">() == 10);
  CHECK(sm.value_triggers == 1);

  // Second mutation triggers again
  r = sm.set("value", std::any(20));
  task.resume();

  CHECK(r == Processed);
  CHECK(sm.get<"value">() == 20);
  CHECK(sm.value_triggers == 2);
}

TEST_CASE("Runtime get - bool attribute") {
  struct Machine : RuntimeAttrInstance, HSM<runtime_model, Machine> {};
  Machine sm;

  std::any result = sm.get("flag");
  REQUIRE(result.has_value());
  const bool* p = std::any_cast<bool>(&result);
  REQUIRE(p != nullptr);
  CHECK(*p == false);

  (void)sm.set("flag", std::any(true));
  result = sm.get("flag");
  p = std::any_cast<bool>(&result);
  REQUIRE(p != nullptr);
  CHECK(*p == true);
}
