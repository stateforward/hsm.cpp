#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"
#include <vector>
#include <string>
#include <algorithm>

using namespace hsm;

using TestEventBase = hsm::AnyEvent;
struct Next : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};

struct TestInstance {
  std::vector<std::string> log;
  void add_log(const std::string& msg) { log.push_back(msg); }
  void clear_log() { log.clear(); }
};

void effect_ab(Signal&, TestInstance& i, const TestEventBase&) {
  static_cast<TestInstance&>(i).add_log("effect_ab");
}

constexpr auto effect_model = define(
    "machine", initial(target("/machine/state_a")),
    state("state_a",
          transition(on<Next>(), target("/machine/state_b"), effect(effect_ab))),
    state("state_b"));

TEST_CASE("Behaviors - Transition Effect") {
  struct Machine : TestInstance, HSM<effect_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/machine/state_a");
  sm.clear_log();

  sm.dispatch<Next>();
  task.resume();

  // Effect should run during transition
  CHECK(sm.log.size() == 1);
  CHECK(sm.log[0] == "effect_ab");
  CHECK(sm.state() == "/machine/state_b");
}
