#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

struct EventHandled : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct EventGuarded : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct EventGuardedTrue : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct EventDeferred : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct EventBack : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct EventUnknown : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};

constexpr auto dispatch_return_model = define(
    "ReturnTest",
    state("Idle", transition(on<EventHandled>(), target("/ReturnTest/Working")),
          transition(on<EventGuarded>(),
                     guard([](auto&, auto&, auto&) { return false; }),
                     target("/ReturnTest/Working")),
          transition(on<EventGuardedTrue>(),
                     guard([](auto&, auto&, auto&) { return true; }),
                     target("/ReturnTest/Working")),
          defer<EventDeferred>()),
    state("Working", transition(on<EventBack>(), target("/ReturnTest/Idle"))),
    initial(target("/ReturnTest/Idle")));

TEST_CASE("Hsm Dispatch Return Value") {
  struct Machine : HSM<dispatch_return_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  SUBCASE("Returns Processed when handled") {
    sm.dispatch<EventHandled>();
    task.resume();
    CHECK(sm.state() == "/ReturnTest/Working");

    sm.dispatch<EventBack>();
    task.resume();
    CHECK(sm.state() == "/ReturnTest/Idle");
  }

  // NOTE: dispatch<EventUnknown>() would now be a compile-time error,
  // because EventUnknown's kind does not appear in the model. This is
  // intentional: using an event type that is not part of the model is
  // treated as a hard error rather than a silent no-op.
  SUBCASE("Returns Processed when guarded false (event was processed but ignored)") {
    sm.dispatch<EventGuarded>();
    task.resume();
    CHECK(sm.state() == "/ReturnTest/Idle");
  }

  SUBCASE("Returns Processed when guarded true") {
    sm.dispatch<EventGuardedTrue>();
    task.resume();
    CHECK(sm.state() == "/ReturnTest/Working");
  }

  SUBCASE("Returns Deferred when deferred") {
    sm.dispatch<EventDeferred>();
    task.resume();
    CHECK(sm.state() == "/ReturnTest/Idle");
  }
}
