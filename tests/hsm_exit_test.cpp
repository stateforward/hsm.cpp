#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"
#include <vector>
#include <string>
#include <algorithm>

using namespace hsm;

using TestEventBase = hsm::AnyEvent;
struct Out : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};

struct TestInstance {
  std::vector<std::string> log;

  void add_log(const std::string& msg) { log.push_back(msg); }
  void clear_log() { log.clear(); }
};

void exit_a(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("exit_a");
}

void exit_b(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("exit_b");
}

void entry_other(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("entry_other");
}

constexpr auto hierarchy_exit_model = define(
    "hier_exit_machine", initial(target("/hier_exit_machine/p/c")),
    state("p", exit(exit_a),
          state("c", exit(exit_b),
                transition(on<Out>(), target("/hier_exit_machine/other")))),
    state("other", entry(entry_other)));

TEST_CASE("Hierarchical Exit Order") {
  struct Machine : TestInstance, HSM<hierarchy_exit_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Initial state /p/c
  CHECK(sm.state() == "/hier_exit_machine/p/c");
  sm.clear_log();

  sm.dispatch<Out>();
  task.resume();

  // Exit: child then parent
  CHECK(sm.log.size() == 3);
  CHECK(sm.log[0] == "exit_b");
  CHECK(sm.log[1] == "exit_a");
  CHECK(sm.log[2] == "entry_other");
}
