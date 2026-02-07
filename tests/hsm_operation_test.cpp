#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

#include <string>
#include <tuple>
#include <vector>
#include <functional>
#include <chrono>
#include <type_traits>

using namespace hsm;

namespace {

// --- Models and machines used to validate HSM::call and CallEvent ---

// 1) Basic member operation that increments an internal counter.
// Use a traits/base type so the model can safely reference
// &MemberMachineBase::increment without needing the CRTP machine
// type to be complete yet.
struct MemberMachineBase {
  int counter{0};

  void increment(int value) {
    counter += value;
  }
};

constexpr auto member_op_model = define(
    "member_machine",
    operation("inc", &MemberMachineBase::increment),
    initial(target("/member_machine/idle")),
    state("idle"));

struct MemberMachine : MemberMachineBase, HSM<member_op_model, MemberMachine> {
  using Base = HSM<member_op_model, MemberMachine>;
  using Base::Base;
};

// 2) Static/free-function operation returning a value.
int static_add(int a, int b) {
  return a + b;
}

constexpr auto static_op_model = define(
    "static_machine",
    operation("add", &static_add),
    initial(target("/static_machine/root")),
    state("root"));

struct StaticMachine : HSM<static_op_model, StaticMachine> {
  using Base = HSM<static_op_model, StaticMachine>;
  using Base::Base;
};

// 3) Operation-name behaviors: entry/exit/effect/guard/activity that
// reference model-level operations by name.

// Simple event types used by the behavior-by-name tests.
struct OpTrigger : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct GuardTrigger : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};

struct BehaviorByNameBase {
  int counter{0};
  std::vector<std::string> log;
  bool allowed{false};

  void bump() { ++counter; }
  void log_a() { log.push_back("a"); }
  void log_b() { log.push_back("b"); }
  void log_effect() { log.push_back("effect"); }

  bool can_go() const { return allowed; }
};

// entry("bump") should run the named operation body on entry.
constexpr auto entry_by_name_model = define(
    "entry_by_name",
    operation("bump", &BehaviorByNameBase::bump),
    initial(target("/entry_by_name/idle")),
    state("idle", entry("bump")));

struct EntryByNameMachine
    : BehaviorByNameBase, HSM<entry_by_name_model, EntryByNameMachine> {
  using Base = HSM<entry_by_name_model, EntryByNameMachine>;
  using Base::Base;
};

// effect("log_a", "log_second") should invoke both operations in order on
// transition. The operation names are deliberately different lengths to avoid
// relying on type-only name matching.
constexpr auto effect_by_name_model = define(
    "effect_by_name",
    operation("log_a", &BehaviorByNameBase::log_a),
    operation("log_second", &BehaviorByNameBase::log_b),
    initial(target("/effect_by_name/s")),
    state("s",
          transition(on<OpTrigger>(),
                     effect("log_a", "log_second"),
                     target("/effect_by_name/s"))));

struct EffectByNameMachine
    : BehaviorByNameBase, HSM<effect_by_name_model, EffectByNameMachine> {
  using Base = HSM<effect_by_name_model, EffectByNameMachine>;
  using Base::Base;
};

// guard("can_go") and effect("log_effect") wired purely by operation name.
constexpr auto guard_by_name_model = define(
    "guard_by_name",
    operation("can_go", &BehaviorByNameBase::can_go),
    operation("log_effect", &BehaviorByNameBase::log_effect),
    initial(target("/guard_by_name/idle")),
    state("idle",
          transition(on<GuardTrigger>(),
                     guard("can_go"),
                     effect("log_effect"),
                     target("/guard_by_name/done"))),
    state("done"));

struct GuardByNameMachine
    : BehaviorByNameBase, HSM<guard_by_name_model, GuardByNameMachine> {
  using Base = HSM<guard_by_name_model, GuardByNameMachine>;
  using Base::Base;
};

// Activity-by-name test: activity("do_work") should run the named operation
// body when entering the state. With native coroutines, this runs synchronously.

struct ActivityByNameBase {
  int runs{0};
  void do_work() { ++runs; }
};

constexpr auto activity_by_name_model = define(
    "activity_by_name",
    operation("do_work", &ActivityByNameBase::do_work),
    initial(target("/activity_by_name/working")),
    state("working", activity("do_work")));

struct ActivityByNameMachine
    : ActivityByNameBase,
      HSM<activity_by_name_model, ActivityByNameMachine> {
  using Base = HSM<activity_by_name_model, ActivityByNameMachine>;
  using Base::Base;
};

// 6) Named-event model for events<Name> mapping tests.
struct NamedEventMachineBase {
  int power_on_count{0};
};

inline void on_power_on(Signal&, NamedEventMachineBase &m, const hsm::AnyEvent &) {
  ++m.power_on_count;
}

constexpr auto named_event_model = define(
    "named_event_machine",
    initial(target("/named_event_machine/idle")),
    state("idle",
          transition(on("power_on"),
                     effect(on_power_on),
                     target("/named_event_machine/idle"))));

struct NamedEventMachine
    : NamedEventMachineBase, HSM<named_event_model, NamedEventMachine> {
  using Base = HSM<named_event_model, NamedEventMachine>;
  using Base::Base;
};

}  // namespace

TEST_CASE("Operations - member operation call executes body") {
  static_assert(MemberMachine::template supports_operation<"inc">());

  MemberMachine sm;
  auto task = sm.start();
  CHECK(sm.counter == 0);

  sm.call<"inc">(5);
  task.resume();
  CHECK(sm.counter == 5);

  sm.call<"inc">(7);
  task.resume();
  CHECK(sm.counter == 12);
}

TEST_CASE("Operations - static/free function operations work and return value") {
  static_assert(StaticMachine::template supports_operation<"add">());

  StaticMachine sm;
  auto task = sm.start();
  auto result1 = sm.call<"add">(2, 3);
  task.resume();
  CHECK(result1 == 5);

  auto result2 = sm.call<"add">(-1, 4);
  task.resume();
  CHECK(result2 == 3);
}

TEST_CASE("Operations - entry by operation name runs body on entry") {
  EntryByNameMachine sm;
  [[maybe_unused]] auto task = sm.start();
  CHECK(sm.state() == "/entry_by_name/idle");
  CHECK(sm.counter == 1);  // entry("bump") invoked &BehaviorByNameBase::bump
}

TEST_CASE("Operations - effect by operation names runs all in order") {
  EffectByNameMachine sm;
  auto task = sm.start();
  CHECK(sm.log.empty());

  sm.dispatch<OpTrigger>();
  task.resume();

  REQUIRE(sm.log.size() == 2);
  CHECK(sm.log[0] == "a");
  CHECK(sm.log[1] == "b");
}

TEST_CASE("Operations - guard by operation name controls transition") {
  GuardByNameMachine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/guard_by_name/idle");

  // Guard returns false by default -> transition must not fire.
  sm.allowed = false;
  sm.dispatch<GuardTrigger>();
  task.resume();
  CHECK(sm.state() == "/guard_by_name/idle");
  CHECK(sm.log.empty());

  // When guard operation returns true, transition should fire and
  // effect("log_effect") should run.
  sm.allowed = true;
  sm.dispatch<GuardTrigger>();
  task.resume();
  CHECK(sm.state() == "/guard_by_name/done");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "effect");
}

TEST_CASE("Operations - activity by operation name runs body on entry") {
  // With native coroutines, the activity runs synchronously when entering.
  ActivityByNameMachine sm;
  [[maybe_unused]] auto task = sm.start();
  CHECK(sm.state() == "/activity_by_name/working");

  // Activity should have run immediately on entry (synchronous execution).
  CHECK(sm.runs == 1);
}
TEST_CASE("Events - events<Name> maps named events to Kind::Event") {
  using M = NamedEventMachine;

  using E = typename M::template events<"power_on">::type;

  static_assert(!M::template events<"power_on">::is_operation);
  static_assert(!M::template events<"power_on">::is_attribute);
  static_assert(M::template events<"power_on">::is_plain_event);

  static_assert(hsm::is_kind(E::kind, hsm::Kind::Event));
  static_assert(!hsm::is_kind(E::kind, hsm::Kind::ChangeEvent));
  static_assert(!hsm::is_kind(E::kind, hsm::Kind::CallEvent));

  static_assert(M::template events<"power_on">::supported());

  // Runtime sanity check: dispatching the named event should hit the effect.
  M sm;
  auto task = sm.start();
  CHECK(sm.power_on_count == 0);
  sm.dispatch<"power_on">();
  task.resume();
  CHECK(sm.power_on_count == 1);
}

TEST_CASE("Events - unsupported name yields supported() == false") {
  using M = StaticMachine;

  using E = typename M::template events<"no_such_event">::type;
  (void) sizeof(E); // suppress unused-type warnings

  static_assert(M::template events<"no_such_event">::is_plain_event);
  static_assert(!M::template events<"no_such_event">::supported());
}
