#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/group.hpp"
#include "hsm/hsm.hpp"

namespace {

struct Go : hsm::Event<hsm::MakeKind(900, hsm::Kind::Event)> {};

constexpr auto pascal_model = hsm::Define(
  "Pascal",
  hsm::Attribute<int>("count", 0),
  hsm::Initial(hsm::Target("/Pascal/idle")),
  hsm::State(
    "idle",
    hsm::Transition(
      hsm::On<Go>(),
      hsm::Effect([](auto &, auto &machine, const auto &) {
        machine.template set<"count">(1);
      }),
      hsm::Target("/Pascal/done"))),
  hsm::Final("done"));

struct PascalMachine : hsm::HSM<pascal_model, PascalMachine> {
  using Base = hsm::HSM<pascal_model, PascalMachine>;
  using Base::Base;
};

}  // namespace

TEST_CASE("PascalCase DSL aliases compile and forward to C++ implementation") {
  PascalMachine machine;
  auto task = machine.start();

  CHECK(machine.state() == "/Pascal/idle");
  machine.dispatch<Go>();
  task.resume();
  CHECK(machine.state() == "/Pascal/done");
  CHECK(machine.template get<"count">() == 1);

  static_assert(hsm::IsKind(Go::kind, hsm::Kind::Event));
}

TEST_CASE("TakeSnapshot exposes bounded normative runtime metadata") {
  PascalMachine machine("pascal-id");

  machine.dispatch<Go>();

  auto snapshot = hsm::TakeSnapshot(machine);
  auto member_snapshot = machine.TakeSnapshot();
  hsm::Signal ctx;
  auto ctx_snapshot = hsm::TakeSnapshot(ctx, machine);

  CHECK(snapshot.ID == "pascal-id");
  CHECK(snapshot.QualifiedName == "/Pascal");
  CHECK(snapshot.State == "");
  CHECK(snapshot.QueueLen == 1);
  REQUIRE(snapshot.EventLen == 1);
  CHECK(snapshot.Events[0].Kind == Go::kind);
  CHECK(snapshot.Events[0].Target == "/Pascal/done");
  CHECK(snapshot.Events[0].Guard == false);

  REQUIRE(snapshot.AttributeLen == 1);
  CHECK(snapshot.Attributes[0].name() == "/Pascal/count");
  REQUIRE(snapshot.Attributes[0].Value != nullptr);
  CHECK(*static_cast<const int *>(snapshot.Attributes[0].Value) == 0);

  CHECK(member_snapshot.ID == snapshot.ID);
  CHECK(member_snapshot.QualifiedName == snapshot.QualifiedName);
  CHECK(ctx_snapshot.QueueLen == snapshot.QueueLen);
}

TEST_CASE("PascalCase group aliases compile") {
  PascalMachine left;
  PascalMachine right;
  auto group = hsm::MakeGroup("group", left, right);

  CHECK(group.id() == "group");
}
