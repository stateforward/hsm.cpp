#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

using namespace hsm;

struct Start : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct Stop : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};
struct Done : hsm::Event<hsm::make_kind(12, hsm::Kind::Event)> {};
struct Next : hsm::Event<hsm::make_kind(13, hsm::Kind::Event)> {};
struct Reset : hsm::Event<hsm::make_kind(14, hsm::Kind::Event)> {};
struct ToB : hsm::Event<hsm::make_kind(15, hsm::Kind::Event)> {};
struct ExitEvent : hsm::Event<hsm::make_kind(16, hsm::Kind::Event)> {};
struct BackDeep : hsm::Event<hsm::make_kind(17, hsm::Kind::Event)> {};
struct ToY : hsm::Event<hsm::make_kind(18, hsm::Kind::Event)> {};
struct BackShallow : hsm::Event<hsm::make_kind(19, hsm::Kind::Event)> {};
struct Go : hsm::Event<hsm::make_kind(20, hsm::Kind::Event)> {};
struct Foo : hsm::Event<hsm::make_kind(21, hsm::Kind::Event)> {};
struct Bar : hsm::Event<hsm::make_kind(22, hsm::Kind::Event)> {};
struct Finish : hsm::Event<hsm::make_kind(23, hsm::Kind::Event)> {};
struct Interrupt : hsm::Event<hsm::make_kind(24, hsm::Kind::Event)> {};
struct Resume : hsm::Event<hsm::make_kind(25, hsm::Kind::Event)> {};
struct Restart : hsm::Event<hsm::make_kind(26, hsm::Kind::Event)> {};

// Models lifted to namespace scope so they can be used as non-type
// template parameters for the CRTP HSM<model, Self, ...>.
constexpr auto simple_model = define("machine");

constexpr auto initial_with_target_model = define(
    "machine", initial(target("/machine/idle")), state("idle"), state("active"));

constexpr auto simple_transition_model = define(
    "machine", initial(target("/machine/idle")),
    state("idle", transition(on<Start>(), target("/machine/active"))),
    state("active"));

constexpr auto hierarchical_model = define(
    "machine", initial(target("/machine/idle")),
    state("idle", transition(on<Start>(), target("/machine/working"))),
    state("working", initial(target("/machine/working/processing")),
          state("processing",
                transition(on<Done>(), target("/machine/idle"))),
          state("waiting")));

constexpr auto sibling_model = define(
    "machine", initial(target("/machine/s1")),
    state("s1", transition(on<Next>(), target("/machine/s2"))),
    state("s2", transition(on<Next>(), target("/machine/s3"))),
    state("s3", transition(on<Reset>(), target("/machine/s1"))));

constexpr auto nested_initial_model = define(
    "machine", initial(target("/machine/outer")),
    state("outer", initial(target("/machine/outer/inner")),
          state("inner", initial(target("/machine/outer/inner/leaf")),
                state("leaf"))));

constexpr auto history_deep_model = define(
    "HistoryDeep", initial(target("/HistoryDeep/region/A")),
    state("region",
          state("A",
                transition(on<ToB>(), target("/HistoryDeep/region/B"))),
          state("B", transition(on<ExitEvent>(), target("/HistoryDeep/outside")))),
    state("outside",
          transition(on<BackDeep>(),
                     target(deep_history("/HistoryDeep/region")))));

constexpr auto history_shallow_model = define(
    "HistoryShallow", initial(target("/HistoryShallow/C/X")),
    state("C", initial(target("/HistoryShallow/C/X")),
          state("X", initial(target("/HistoryShallow/C/X/X1")),
                state("X1",
                      transition(on<ToY>(),
                                 target("/HistoryShallow/C/Y"))),
                state("X2")),
          state("Y",
                transition(on<ExitEvent>(), target("/HistoryShallow/outside")))),
    state("outside",
          transition(on<BackShallow>(),
                     target(shallow_history("/HistoryShallow/C")))));

constexpr auto history_no_prior_model = define(
    "HistoryNoPrior", initial(target("/HistoryNoPrior/outside")),
    state("C", initial(target("/HistoryNoPrior/C/A")), state("A"), state("B")),
    state("outside",
          transition(on<Go>(),
                     target(shallow_history("/HistoryNoPrior/C")))));

constexpr auto wildcard_model = define(
    "WildcardMachine", initial(target("/WildcardMachine/s")),
    state("s", transition(on<Foo>(), target("/WildcardMachine/foo_state")),
          transition(on<AnyEvent>(), target("/WildcardMachine/any_state"))),
    state("foo_state"), state("any_state"));

constexpr auto completion_comp_model = define(
    "CompMachine", initial(target("/CompMachine/container/start")),
    state("container", transition(target("/CompMachine/finished")),
          state("start",
                transition(on<Go>(),
                           target("/CompMachine/container/end"))),
          final("end")),
    state("finished"));

using NamedGo = hsm::detail::named_event<
    hsm::detail::make_fixed_string("go_named")>;

constexpr auto named_event_model = define(
    "NamedEventMachine", initial(target("/NamedEventMachine/idle")),
    state("idle",
          transition(on<NamedGo>(),
                     target("/NamedEventMachine/active"))),
    state("active"));

constexpr auto literal_dispatch_model = define(
    "LiteralDispatchMachine",
    initial(target("/LiteralDispatchMachine/idle")),
    state("idle", transition(on("fire"),
                              target("/LiteralDispatchMachine/active"))),
    state("active"));

// Named event type alias with hierarchical name for matching on("foo.bar")
constexpr auto foo_bar_name = detail::make_fixed_string("foo.bar");
using foo_bar = detail::named_event<foo_bar_name>;

constexpr auto named_event_alias_model = define(
    "NamedEventAliasMachine",
    initial(target("/NamedEventAliasMachine/idle")),
    state("idle", transition(on("foo.bar"),
                              target("/NamedEventAliasMachine/active"))),
    state("active"));

constexpr auto hist_comp_model = define(
    "HistComp", initial(target("/HistComp/container/step1")),
    state("container", transition(target("/HistComp/completed")),
          state("step1",
                transition(on<Next>(),
                           target("/HistComp/container/step2"))),
          state("step2",
                transition(on<Finish>(),
                           target("/HistComp/container/done")),
                transition(on<Interrupt>(),
                           target("/HistComp/interrupted"))),
          final("done")),
    state("completed",
          transition(on<Restart>(), target("/HistComp/container"))),
    state("interrupted",
          transition(on<Resume>(),
                     target(shallow_history("/HistComp/container")))));

TEST_CASE("Dispatch - Initial State") {
  // Simple machine without initial -> stays at root
  struct Machine : HSM<simple_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/machine");
}

TEST_CASE("Dispatch - Initial With Target") {
  struct Machine : HSM<initial_with_target_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/machine/idle");
}

TEST_CASE("Dispatch - Simple Transition") {
  struct Machine : HSM<simple_transition_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/machine/idle");

  sm.dispatch<Start>();
  task.resume();
  CHECK(sm.state() == "/machine/active");
}

TEST_CASE("Dispatcher - HSM runtime polymorphic dispatch") {
  struct Machine : HSM<simple_transition_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  Instance *disp = &sm;

  CHECK(sm.state() == "/machine/idle");

  Start ev;
  disp->dispatch(ev);
  task.resume();
  CHECK(sm.state() == "/machine/active");
}

// NOTE: A previous version of this suite verified that dispatching an
// event type that was completely unknown to the model (no matching
// transitions and no wildcard) would be ignored at runtime. The HSM now
// treats such usage as a compile-time error: if there is neither a
// concrete transition nor an AnyEvent wildcard for an event kind, using
// that event type with dispatch<T>() is rejected during compilation.

TEST_CASE("Dispatch - Hierarchical Transition") {
  struct Machine : HSM<hierarchical_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/machine/idle");

  sm.dispatch<Start>();
  task.resume();
  CHECK(sm.state() == "/machine/working/processing");

  sm.dispatch<Done>();
  task.resume();
  CHECK(sm.state() == "/machine/idle");
}

TEST_CASE("Dispatch - Sibling Transitions") {
  struct Machine : HSM<sibling_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/machine/s1");

  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/machine/s2");

  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/machine/s3");

  sm.dispatch<Reset>();
  task.resume();
  CHECK(sm.state() == "/machine/s1");
}

TEST_CASE("Dispatch - Nested Initial Transitions") {
  struct Machine : HSM<nested_initial_model, Machine> {};
  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/machine/outer/inner/leaf");
}

TEST_CASE("Dispatch - History pseudostates") {
struct HistoryInstance {};

  SUBCASE("Deep history returns to exact leaf") {
    struct Machine : HistoryInstance,
                     HSM<history_deep_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/HistoryDeep/region/A");

    sm.dispatch<ToB>();
    task.resume();
    CHECK(sm.state() == "/HistoryDeep/region/B");

    sm.dispatch<ExitEvent>();
    task.resume();
    CHECK(sm.state() == "/HistoryDeep/outside");

    sm.dispatch<BackDeep>();
    task.resume();
    CHECK(sm.state() == "/HistoryDeep/region/B");
  }

  SUBCASE(
      "Shallow history re-enters last active child and follows its initial") {
    struct Machine : HistoryInstance,
                     HSM<history_shallow_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/HistoryShallow/C/X/X1");

    sm.dispatch<ToY>();
    task.resume();
    CHECK(sm.state() == "/HistoryShallow/C/Y");

    sm.dispatch<ExitEvent>();
    task.resume();
    CHECK(sm.state() == "/HistoryShallow/outside");

    sm.dispatch<BackShallow>();
    task.resume();
    CHECK(sm.state() == "/HistoryShallow/C/Y");
  }

  SUBCASE("History with no prior leaf falls back to composite initial") {
    struct Machine : HistoryInstance,
                     HSM<history_no_prior_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/HistoryNoPrior/outside");

    sm.dispatch<Go>();
    task.resume();
    CHECK(sm.state() == "/HistoryNoPrior/C/A");
  }
}

TEST_CASE("Dispatch - Wildcard transitions") {
struct WildInstance {};

  struct Machine : WildInstance,
                   HSM<wildcard_model, Machine> {};

  SUBCASE("Wildcard handles unknown event in same state") {
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/WildcardMachine/s");

    sm.dispatch<Bar>();
    task.resume();
    CHECK(sm.state() == "/WildcardMachine/any_state");
  }

  SUBCASE("Specific event wins over wildcard") {
    Machine sm;
    auto task = sm.start();

    CHECK(sm.state() == "/WildcardMachine/s");

    sm.dispatch<Foo>();
    task.resume();
    CHECK(sm.state() == "/WildcardMachine/foo_state");
  }
}

TEST_CASE("Dispatch - Completion transitions (Composite)") {
struct CompInstance {};

  struct Machine : CompInstance,
                   HSM<completion_comp_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/CompMachine/container/start");

  sm.dispatch<Go>();
  task.resume();
  // Should go to end, then immediately triggers completion on container ->
  // finished
  CHECK(sm.state() == "/CompMachine/finished");
}

TEST_CASE("Dispatch - Named events and string literal dispatch") {
  struct NamedMachine : HSM<named_event_model, NamedMachine> {};
  NamedMachine sm_named;
  auto task_named = sm_named.start();
  CHECK(sm_named.state() == "/NamedEventMachine/idle");

  sm_named.dispatch<NamedGo>();
  task_named.resume();
  CHECK(sm_named.state() == "/NamedEventMachine/active");

  struct LiteralMachine : HSM<literal_dispatch_model, LiteralMachine> {};
  LiteralMachine sm_lit;
  auto task_lit = sm_lit.start();
  CHECK(sm_lit.state() == "/LiteralDispatchMachine/idle");

  // Known literal should be handled.
  sm_lit.dispatch<"fire">();
  task_lit.resume();
  CHECK(sm_lit.state() == "/LiteralDispatchMachine/active");

  // NOTE: dispatch<"unknown">() would now be a compile-time error,
  // because the literal "unknown" does not appear in the model. Literal
  // dispatch is fully typed and requires that all event kinds are part
  // of the model; unknown literals are treated as hard errors rather
  // than silent no-ops.
}

TEST_CASE("Dispatch - named_event type alias matches on<string> handler") {
  // Verify that named_event<"foo.bar"> dispatched as instance matches on<"foo.bar">()
  struct Machine : HSM<named_event_alias_model, Machine> {};

  Machine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/NamedEventAliasMachine/idle");

  // Dispatch using named_event instance - should match on<"foo.bar">()
  sm.dispatch(foo_bar{});
  task.resume();
  CHECK(sm.state() == "/NamedEventAliasMachine/active");
}

TEST_CASE("Dispatch - History and Completion interaction") {
struct HistCompInstance {};

  struct Machine : HistCompInstance,
                   HSM<hist_comp_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/HistComp/container/step1");

  // 1. Interrupt and Resume (History)
  sm.dispatch<Next>();
  task.resume();
  CHECK(sm.state() == "/HistComp/container/step2");

  sm.dispatch<Interrupt>();
  task.resume();
  CHECK(sm.state() == "/HistComp/interrupted");

  sm.dispatch<Resume>();
  task.resume();
  // Should resume to step2
  CHECK(sm.state() == "/HistComp/container/step2");

  // 2. Finish and Complete
  sm.dispatch<Finish>();
  task.resume();
  // step2 -> done (final) -> container completes -> completed
  CHECK(sm.state() == "/HistComp/completed");
}
