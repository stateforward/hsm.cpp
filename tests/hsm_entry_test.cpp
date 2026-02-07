#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"
#include <vector>
#include <string>
#include <algorithm>

using namespace hsm;

using TestEventBase = hsm::AnyEvent;

struct TestInstance {
  std::vector<std::string> log;

  void add_log(const std::string& msg) { log.push_back(msg); }
};

void entry_a(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("entry_a");
}

void entry_b(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("entry_b");
}

constexpr auto entry_model = define(
    "machine", initial(target("/machine/state_a/state_b")),
    state("state_a", entry(entry_a),
          initial(target("/machine/state_a/state_b")),
          state("state_b", entry(entry_b))));

TEST_CASE("Entry Behaviors") {
  struct Machine : TestInstance, HSM<entry_model, Machine> {};
  Machine sm;
  sm.start();

  // Check initial entry order: entry_a (parent) then entry_b (child)
  CHECK(sm.state() == "/machine/state_a/state_b"); // wait, target is state_a, but it has substate?
  // entry_model invalid? state_a has state_b inside. initial target is state_a.
  // If state_a is composite, it must have initial or we stay at state_a (if allowed) or error?
  // hsm treats state("state_a", ..., state("state_b")) as composite.
  // If target is state_a, and it has children, we enter state_a.
  // But strictly, we should enter a leaf or define initial for state_a.
  // Wait, `entry_model` above:
  // initial(target("/machine/state_a"))
  // state("state_a", ..., state("state_b"))
  // If I target state_a, do I enter state_b? No, unless state_a has initial -> state_b.
  // My defined model in `hsm_behavior_test.cpp` was `hierarchy_order_model`: initial(target("/machine/p/c")).
  // So I should target the child to test hierarchical entry.

  // Let's correct the model.
}

constexpr auto hierarchy_entry_model = define(
    "hier_entry_machine", initial(target("/hier_entry_machine/p/c")),
    state("p", entry(entry_a),
          state("c", entry(entry_b))));

TEST_CASE("Hierarchical Entry Order") {
  struct Machine : TestInstance, HSM<hierarchy_entry_model, Machine> {};
  Machine sm;
  sm.start();

  // Entry: parent then child
  CHECK(sm.state() == "/hier_entry_machine/p/c");
  CHECK(sm.log.size() == 2);
  CHECK(sm.log[0] == "entry_a");
  CHECK(sm.log[1] == "entry_b");
}
