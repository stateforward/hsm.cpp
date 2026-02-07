#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

#include <string>
#include <tuple>
#include <vector>
#include <functional>
#include <type_traits>

using namespace hsm;

namespace {

// --- CallEvent and on_call() tests ---

// 1) CallEvent-based model that logs an effect before the operation body.
struct CallLogMachineBase {
  std::vector<std::string> log;
  int last_arg{0};
  int body_arg{0};

  void do_impl(int value) {
    log.push_back("call");
    body_arg = value;
  }
};

using DoEvent = CallEvent<detail::make_fixed_string("do"), std::tuple<int>>;

inline void on_call_event(hsm::Signal&, CallLogMachineBase& m, const DoEvent& evt) {
  m.log.push_back("event");
  m.last_arg = std::get<0>(evt.args);
}

constexpr auto call_event_model = define(
    "call_machine",
    operation("do", &CallLogMachineBase::do_impl),
    initial(target("/call_machine/idle")),
    state("idle",
          transition(on<DoEvent>(),
                     effect(on_call_event),
                     target("/call_machine/idle"))));

struct CallLogMachine : CallLogMachineBase, HSM<call_event_model, CallLogMachine> {
  using Base = HSM<call_event_model, CallLogMachine>;
  using Base::Base;
};

// 2) on_call-based model: transition listens for CallEvent-kind
// events keyed by the operation name and fires before the
// operation body runs.
struct OnCallMachineBase {
  std::vector<std::string> log;
  int last_body_arg{0};

  void do_impl(int value) {
    log.push_back("body");
    last_body_arg = value;
  }
};

inline void on_do_something(OnCallMachineBase &self) {
  self.log.push_back("effect");
}

constexpr auto on_call_model = define(
    "on_call_machine",
    operation("do_something", &OnCallMachineBase::do_impl),
    initial(target("/on_call_machine/idle")),
    state("idle",
          transition(
              on_call("do_something"),
              effect(on_do_something),
              target("/on_call_machine/idle"))));

struct OnCallMachine : OnCallMachineBase, HSM<on_call_model, OnCallMachine> {
  using Base = HSM<on_call_model, OnCallMachine>;
  using Base::Base;
};

} // namespace

TEST_CASE("Operations - CallEvent dispatched before operation body") {
  static_assert(CallLogMachine::template supports_operation<"do">());

  CallLogMachine sm;
  auto task = sm.start();
  sm.log.clear();

  sm.call<"do">(42);
  task.resume();

  REQUIRE(sm.log.size() == 2);
  CHECK(sm.log[0] == "event");
  CHECK(sm.log[1] == "call");
  CHECK(sm.last_arg == 42);
  CHECK(sm.body_arg == 42);
}

TEST_CASE("Operations - on_call transitions are driven by HSM::call") {
  // Compile-time guarantees: operation is declared and the
  // corresponding CallEvent kind is present in the model.
  static_assert(OnCallMachine::template supports_operation<"do_something">());
  using DoSomethingEvent = CallEvent<
      hsm::detail::make_fixed_string("do_something"),
      std::tuple<int>>;
  static_assert(OnCallMachine::template supports_event<DoSomethingEvent>());

  OnCallMachine sm;
  auto task = sm.start();
  sm.log.clear();

  sm.call<"do_something">(7);
  task.resume();

  REQUIRE(sm.log.size() == 2);
  CHECK(sm.log[0] == "effect");
  CHECK(sm.log[1] == "body");
  CHECK(sm.last_body_arg == 7);
}

TEST_CASE("Events - events<Name> maps operations to CallEvent") {
  using M = OnCallMachine;

  using E = typename M::template events<"do_something">::type;

  static_assert(M::template events<"do_something">::is_operation);
  static_assert(!M::template events<"do_something">::is_attribute);
  static_assert(!M::template events<"do_something">::is_plain_event);

  using ExpectedEvent = CallEvent<
      hsm::detail::make_fixed_string("do_something"),
      std::tuple<int>>;

  static_assert(std::is_same_v<E, ExpectedEvent>,
                "events<Name>::type must alias the operation CallEvent type");

  static_assert(M::template events<"do_something">::supported());
}
