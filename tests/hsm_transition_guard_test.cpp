#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"
#include <vector>
#include <string>

using namespace hsm;

using TestEventBase = hsm::AnyEvent;
struct Go : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};

struct TestInstance {
  std::vector<std::string> log;
  void add_log(const std::string& msg) { log.push_back(msg); }
};

bool guard_true(Signal&, TestInstance&, const TestEventBase&) { return true; }
bool guard_false(Signal&, TestInstance&, const TestEventBase&) { return false; }

constexpr auto guards_model = define(
    "machine", initial(target("/machine/start")),
    state("start",
          // High priority transition blocked by guard
          transition(on<Go>(), guard(guard_false), target("/machine/blocked")),
          // Fallback transition allowed
          transition(on<Go>(), guard(guard_true), target("/machine/allowed"))),
    state("blocked"), state("allowed"));

TEST_CASE("Behaviors - Guards Priority") {
  struct Machine : TestInstance, HSM<guards_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Should skip the first transition (guard_false) and take the second (guard_true)
  sm.dispatch<Go>();
  task.resume();
  CHECK(sm.state() == "/machine/allowed");
}
