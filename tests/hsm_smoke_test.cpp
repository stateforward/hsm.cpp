#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

struct NoOp : hsm::Event<hsm::event_kind("NoOp")> {};


constexpr auto smoke_model = hsm::define("smoke");

TEST_CASE("hsm smoke test compiles and exposes basic APIs") {
  struct Machine : hsm::HSM<smoke_model, Machine> {};
  Machine machine;
  machine.start();

  CHECK(machine.state() == "/smoke");
  MESSAGE("NoOp hash: " << hsm::detail::fnv1a_64("NoOp"));
  static_assert(hsm::is_kind(NoOp::kind, hsm::Kind::Event));
  MESSAGE("NoOp kind: " << NoOp::kind);
  // NOTE: In the updated design, dispatching an event type whose kind does not
  // appear in the model (and is not covered by an AnyEvent wildcard) is a
  // compile-time error. This smoke test now only verifies basic construction
  // and type relationships; it no longer dispatches NoOp.
}



constexpr auto test_model = hsm::define("test");

TEST_CASE("hsm constructor overloads work") {
  struct Machine : hsm::HSM<test_model, Machine> {
    using Base = hsm::HSM<test_model, Machine>;
    using Base::Base;
  };

  // Test default constructor
  Machine machine;
  machine.start();
  CHECK(machine.state() == "/test");
}
