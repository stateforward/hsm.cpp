#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

using namespace hsm;

namespace {

struct StartEvent
    : hsm::Event<hsm::make_kind(100, hsm::Kind::Event)> {};
struct PayloadEvent
    : hsm::Event<hsm::make_kind(101, hsm::Kind::Event)> {
  int value{};
};
struct UnrelatedEvent : hsm::Event<hsm::make_kind(102, hsm::Kind::Event)> {};

struct Device {
  int runtime_entries = 0;
  int typed_entries = 0;
  int guard_checks = 0;
  int effect_calls = 0;
  int payload_sum = 0;

  // invoke_typed matrix counters
  int full_sig_calls = 0;        // F(Signal&, Device&, const StartEvent&)
  int inst_event_calls = 0;      // F(Device&, const StartEvent&)
  int inst_only_calls = 0;       // F(Device&)
  int any_event_calls = 0;       // F(Signal&, Device&, const AnyEvent&)
  int payload_only_calls = 0;    // F(Device&, const PayloadEvent&)
};

// Global counter for a behavior with no parameters at all. This exercises the
// invoke_typed branch that calls F() with no arguments.
static int g_noarg_calls = 0;

// Behavior variants used to exercise the invoke_typed matrix
void full_sig_handler(hsm::Signal&, Device& d, const StartEvent&) {
  d.full_sig_calls++;
}

void inst_event_handler(Device& d, const StartEvent&) {
  d.inst_event_calls++;
}

void inst_only_handler(Device& d) {
  d.inst_only_calls++;
}

void noarg_handler() {
  ++g_noarg_calls;
}

void any_event_handler(hsm::Signal&, Device& d, const hsm::AnyEvent&) {
  d.any_event_calls++;
}

void payload_only_handler(Device& d, const PayloadEvent&) {
  d.payload_only_calls++;
}

constexpr auto typed_model = define(
    "device", initial(target("/device/idle")),
    state("idle", entry([](hsm::Signal&, Device& d, const hsm::EventBase&) {
            ++d.runtime_entries;
          }),
          entry([](hsm::Signal&, Device& d, const StartEvent&) {
            ++d.typed_entries;
          }),
          transition(
              on<StartEvent>(), guard([](Device& d, const StartEvent&) {
                ++d.guard_checks;
                return true;
              }),
              effect([](Device& d, const StartEvent&) { ++d.effect_calls; }),
              target("/device/idle"))),
    state("active"));

constexpr auto payload_model = define(
    "payload_device", initial(target("/payload_device/idle")),
    state("idle", transition(on<PayloadEvent>(),
                             effect([](Device& d, const PayloadEvent& evt) {
                               d.payload_sum += evt.value;
                             }),
                             target("/payload_device/idle"))));

// Model specifically designed to exercise the invoke_typed resolution matrix.
// It attaches multiple behavior signatures to the same StartEvent transition
// and reuses a subset of them for PayloadEvent to verify which ones fire.
constexpr auto invoke_matrix_model = define(
    "invoke_matrix", initial(target("/invoke_matrix/idle")),
    state("idle",
          // StartEvent transition: all behavior variants are attached.
          transition(on<StartEvent>(),
                     effect(full_sig_handler,
                            inst_event_handler,
                            inst_only_handler,
                            noarg_handler,
                            any_event_handler,
                            payload_only_handler),
                     target("/invoke_matrix/idle")),
          // PayloadEvent transition: only event-agnostic and payload-specific
          // handlers are attached.
          transition(on<PayloadEvent>(),
                     effect(inst_only_handler,
                            noarg_handler,
                            any_event_handler,
                            payload_only_handler),
                     target("/invoke_matrix/idle"))));

}  // namespace

TEST_CASE("Typed events dispatch and behavior resolution") {
  struct Machine : Device, HSM<typed_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  CHECK(sm.runtime_entries == 1);  // initial entry via init event
  CHECK(sm.typed_entries == 0);

  sm.dispatch<StartEvent>();
  task.resume();
  CHECK(sm.typed_entries == 1);
  CHECK(sm.guard_checks == 1);
  CHECK(sm.effect_calls == 1);
  CHECK(sm.runtime_entries == 2);  // entry invoked again for typed event
  CHECK(sm.state() == "/device/idle");
}

// NOTE: In the updated design, dispatching an event type that does not
// appear anywhere in the model (and is not covered by an AnyEvent
// wildcard) is a compile-time error rather than a runtime no-op. The
// previous test that dispatched UnrelatedEvent has therefore been
// removed.

TEST_CASE("invoke_typed behavior matrix - StartEvent") {
  struct Machine : Device, HSM<invoke_matrix_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  // Clear all counters
  sm.full_sig_calls = 0;
  sm.inst_event_calls = 0;
  sm.inst_only_calls = 0;
  sm.any_event_calls = 0;
  sm.payload_only_calls = 0;
  g_noarg_calls = 0;

  sm.dispatch<StartEvent>();
  task.resume();

  CHECK(sm.full_sig_calls == 1);
  CHECK(sm.inst_event_calls == 1);
  CHECK(sm.inst_only_calls == 1);
  CHECK(sm.any_event_calls == 1);
  CHECK(sm.payload_only_calls == 0); // payload-only handler not compatible
  CHECK(g_noarg_calls == 1);
}

TEST_CASE("invoke_typed behavior matrix - PayloadEvent") {
  struct Machine : Device, HSM<invoke_matrix_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  sm.full_sig_calls = 0;
  sm.inst_event_calls = 0;
  sm.inst_only_calls = 0;
  sm.any_event_calls = 0;
  sm.payload_only_calls = 0;
  g_noarg_calls = 0;

  PayloadEvent payload{};
  payload.value = 42;
  sm.dispatch(payload);
  task.resume();

  // Only event-agnostic and payload-specific handlers should fire.
  CHECK(sm.full_sig_calls == 0);
  CHECK(sm.inst_event_calls == 0);
  CHECK(sm.inst_only_calls == 1);
  CHECK(sm.any_event_calls == 1);
  CHECK(sm.payload_only_calls == 1);
  CHECK(g_noarg_calls == 1);
}

TEST_CASE("Typed dispatch forwards event payloads") {
  struct Machine : Device, HSM<payload_model, Machine> {};
  Machine sm;
  auto task = sm.start();

  PayloadEvent payload{};
  payload.value = 5;
  sm.dispatch(payload);
  task.resume();
  CHECK(sm.payload_sum == 5);

  PayloadEvent payload2{};
  payload2.value = 7;
  sm.dispatch(payload2);
  task.resume();
  CHECK(sm.payload_sum == 12);
}
