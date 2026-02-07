#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

namespace {

// ============================================================================
// Events with attributes
// ============================================================================

struct Login : hsm::Event<hsm::make_kind(200, hsm::Kind::Event)> {
  int user_id{};
  int role{};  // 0=user, 1=admin
};

struct Transfer : hsm::Event<hsm::make_kind(201, hsm::Kind::Event)> {
  int amount{};
  int destination{};
};

struct Logout : hsm::Event<hsm::make_kind(202, hsm::Kind::Event)> {};

struct Deposit : hsm::Event<hsm::make_kind(203, hsm::Kind::Event)> {
  int amount{};
};

struct SetLevel : hsm::Event<hsm::make_kind(204, hsm::Kind::Event)> {
  int level{};
};

struct Command : hsm::Event<hsm::make_kind(205, hsm::Kind::Event)> {
  int code{};
  int arg{};
};

struct Advance : hsm::Event<hsm::make_kind(206, hsm::Kind::Event)> {
  int step{};
};

struct Reset : hsm::Event<hsm::make_kind(207, hsm::Kind::Event)> {};

struct Configure : hsm::Event<hsm::make_kind(208, hsm::Kind::Event)> {
  int mode{};
  int threshold{};
};

struct Trigger : hsm::Event<hsm::make_kind(209, hsm::Kind::Event)> {
  int value{};
};

struct Ping : hsm::Event<hsm::make_kind(210, hsm::Kind::Event)> {
  int seq{};
};

// ============================================================================
// Instance contexts
// ============================================================================

struct Account {
  int logged_in_user{-1};
  int balance{0};
  int last_transfer_dest{-1};
  int last_transfer_amount{0};
  bool admin_logged_in{false};
  int login_count{0};
  int logout_count{0};
};

struct Tracker {
  std::vector<std::string> log;
  int level{0};
  int entry_count{0};
  int exit_count{0};
  int effect_count{0};
  int guard_pass_count{0};
  int guard_fail_count{0};
  int last_code{-1};
  int last_arg{-1};
  int accumulated{0};
  int mode{-1};
  int threshold{-1};
  int last_ping_seq{-1};
};

// ============================================================================
// Model 1: Account (effects, guards, state changes)
// ============================================================================

constexpr auto account_model = define(
    "account",
    initial(target("/account/logged_out")),

    state("logged_out",
          transition(on<Login>(),
                     effect([](Account& a, const Login& e) {
                       a.logged_in_user = e.user_id;
                       a.admin_logged_in = (e.role == 1);
                       a.login_count++;
                     }),
                     target("/account/logged_in"))),

    state("logged_in",
          transition(on<Transfer>(),
                     guard([](Account& a, const Transfer& e) {
                       return e.amount > 0 && e.amount <= a.balance;
                     }),
                     effect([](Account& a, const Transfer& e) {
                       a.balance -= e.amount;
                       a.last_transfer_dest = e.destination;
                       a.last_transfer_amount = e.amount;
                     }),
                     target("/account/logged_in")),
          transition(on<Deposit>(),
                     effect([](Account& a, const Deposit& e) {
                       a.balance += e.amount;
                     }),
                     target("/account/logged_in")),
          transition(on<Logout>(),
                     effect([](Account& a) { a.logout_count++; }),
                     target("/account/logged_out"))));

struct AccountMachine : Account, HSM<account_model, AccountMachine> {};

// ============================================================================
// Model 2: Entry/exit actions receive event attributes
// ============================================================================

constexpr auto entry_exit_model = define(
    "tracker",
    initial(target("/tracker/idle")),

    state("idle",
          entry([](Tracker& t, const hsm::EventBase&) { t.entry_count++; }),
          exit([](Tracker& t, const hsm::EventBase&) { t.exit_count++; }),
          transition(on<SetLevel>(),
                     effect([](Tracker& t, const SetLevel& e) {
                       t.level = e.level;
                       t.effect_count++;
                     }),
                     target("/tracker/active")),
          transition(on<Reset>(), target("/tracker/idle"))),

    state("active",
          entry([](Tracker& t, const SetLevel& e) {
            t.log.push_back("typed_entry:" + std::to_string(e.level));
          }),
          entry([](Tracker& t, const hsm::EventBase&) { t.entry_count++; }),
          exit([](Tracker& t, const hsm::EventBase&) { t.exit_count++; }),
          transition(on<SetLevel>(),
                     effect([](Tracker& t, const SetLevel& e) {
                       t.level = e.level;
                       t.effect_count++;
                     }),
                     target("/tracker/active")),
          transition(on<Reset>(), target("/tracker/idle"))));

struct EntryExitMachine : Tracker, HSM<entry_exit_model, EntryExitMachine> {};

// ============================================================================
// Model 3: Choice pseudostate routes based on event data stored in instance
//
// NOTE: Choice pseudostate guards receive a CompletionEvent, not the
// original triggering event. To route by event data, store the payload
// into instance state via the incoming transition's effect, then read
// it in the choice guards.
// ============================================================================

constexpr auto choice_model = define(
    "router",
    initial(target("/router/waiting")),

    state("waiting",
          transition(on<Command>(),
                     // Store event data into instance before entering choice
                     effect([](Tracker& t, const Command& e) {
                       t.last_code = e.code;
                       t.last_arg = e.arg;
                     }),
                     target("/router/decide"))),

    choice("decide",
           transition(guard([](Tracker& t) {
                        t.guard_pass_count++;
                        return t.last_code == 1;
                      }),
                      target("/router/mode_a")),
           transition(guard([](Tracker& t) {
                        t.guard_pass_count++;
                        return t.last_code == 2;
                      }),
                      target("/router/mode_b")),
           transition(target("/router/error"))),

    state("mode_a"),
    state("mode_b"),
    state("error"));

struct ChoiceMachine : Tracker, HSM<choice_model, ChoiceMachine> {};

// ============================================================================
// Model 4: Internal transition with event attributes (no state change)
// ============================================================================

constexpr auto internal_model = define(
    "counter",
    initial(target("/counter/running")),

    state("running",
          // Internal transition: effect only, no target
          transition(on<Advance>(),
                     effect([](Tracker& t, const Advance& e) {
                       t.accumulated += e.step;
                       t.effect_count++;
                     })),
          transition(on<Reset>(), target("/counter/running"))));

struct InternalMachine : Tracker, HSM<internal_model, InternalMachine> {};

// ============================================================================
// Model 5: Multiple effects on a single transition
// ============================================================================

static void log_configure(Tracker& t, const Configure& e) {
  t.log.push_back("configure:" + std::to_string(e.mode) +
                   ":" + std::to_string(e.threshold));
}

static void apply_mode(Tracker& t, const Configure& e) {
  t.mode = e.mode;
}

static void apply_threshold(Tracker& t, const Configure& e) {
  t.threshold = e.threshold;
}

constexpr auto multi_effect_model = define(
    "multi",
    initial(target("/multi/idle")),

    state("idle",
          transition(on<Configure>(),
                     effect(log_configure, apply_mode, apply_threshold),
                     target("/multi/configured"))),

    state("configured",
          transition(on<Configure>(),
                     effect(log_configure, apply_mode, apply_threshold),
                     target("/multi/configured"))));

struct MultiEffectMachine : Tracker, HSM<multi_effect_model, MultiEffectMachine> {};

// ============================================================================
// Model 6: Hierarchical states - event attributes flow through nesting
// ============================================================================

constexpr auto hierarchy_model = define(
    "hier",
    initial(target("/hier/parent/child_a")),

    state("parent",
          // Parent-level internal transition sees event attributes
          transition(on<Ping>(),
                     effect([](Tracker& t, const Ping& e) {
                       t.last_ping_seq = e.seq;
                       t.log.push_back("parent_ping:" + std::to_string(e.seq));
                     })),

          state("child_a",
                transition(on<SetLevel>(),
                           guard([](const Tracker&, const SetLevel& e) {
                             return e.level > 0;
                           }),
                           effect([](Tracker& t, const SetLevel& e) {
                             t.level = e.level;
                           }),
                           target("/hier/parent/child_b"))),

          state("child_b",
                transition(on<SetLevel>(),
                           effect([](Tracker& t, const SetLevel& e) {
                             t.level = e.level;
                           }),
                           target("/hier/parent/child_a")))));

struct HierarchyMachine : Tracker, HSM<hierarchy_model, HierarchyMachine> {};

// ============================================================================
// Model 7: Deferred events preserve attributes
// ============================================================================

constexpr auto defer_model = define(
    "deferrer",
    initial(target("/deferrer/holding")),

    state("holding",
          defer<Trigger>(),
          transition(on<Advance>(),
                     effect([](Tracker& t, const Advance& e) {
                       t.accumulated += e.step;
                     }),
                     target("/deferrer/processing"))),

    state("processing",
          transition(on<Trigger>(),
                     effect([](Tracker& t, const Trigger& e) {
                       t.accumulated += e.value;
                       t.log.push_back("trigger:" + std::to_string(e.value));
                     }),
                     target("/deferrer/done"))),

    state("done"));

struct DeferMachine : Tracker, HSM<defer_model, DeferMachine> {};

// ============================================================================
// Model 8: Multiple guard candidates - first matching wins with event data
// ============================================================================

constexpr auto multi_guard_model = define(
    "mg",
    initial(target("/mg/idle")),

    state("idle",
          // First guard: value > 100 -> high
          transition(on<Trigger>(),
                     guard([](const Tracker&, const Trigger& e) {
                       return e.value > 100;
                     }),
                     effect([](Tracker& t) {
                       t.log.push_back("high");
                     }),
                     target("/mg/high")),
          // Second guard: value > 0 -> low
          transition(on<Trigger>(),
                     guard([](const Tracker&, const Trigger& e) {
                       return e.value > 0;
                     }),
                     effect([](Tracker& t) {
                       t.log.push_back("low");
                     }),
                     target("/mg/low")),
          // No guard (else): -> zero
          transition(on<Trigger>(),
                     effect([](Tracker& t) {
                       t.log.push_back("zero");
                     }),
                     target("/mg/zero"))),

    state("high"),
    state("low"),
    state("zero"));

struct MultiGuardMachine : Tracker, HSM<multi_guard_model, MultiGuardMachine> {};

// ============================================================================
// Model 9: Wildcard AnyEvent handler receives alongside typed events
// ============================================================================

constexpr auto wildcard_model = define(
    "wild",
    initial(target("/wild/idle")),

    state("idle",
          // Specific handler for Ping
          transition(on<Ping>(),
                     effect([](Tracker& t, const Ping& e) {
                       t.last_ping_seq = e.seq;
                     }),
                     target("/wild/pinged")),
          // Wildcard handler catches anything else
          transition(on<AnyEvent>(),
                     effect([](Tracker& t) {
                       t.log.push_back("wildcard");
                     }),
                     target("/wild/caught"))),

    state("pinged"),
    state("caught"));

struct WildcardMachine : Tracker, HSM<wildcard_model, WildcardMachine> {};

}  // namespace

// ============================================================================
// TEST CASES
// ============================================================================

// --- Model 1: Account basic tests ---

TEST_CASE("Event attributes - effect receives event data") {
  AccountMachine sm;
  auto task = sm.start();
  CHECK(sm.state() == "/account/logged_out");

  Login login{};
  login.user_id = 42;
  login.role = 0;
  sm.dispatch(login);
  task.resume();

  CHECK(sm.state() == "/account/logged_in");
  CHECK(sm.logged_in_user == 42);
  CHECK(sm.admin_logged_in == false);
  CHECK(sm.login_count == 1);
}

TEST_CASE("Event attributes - different payloads produce different results") {
  AccountMachine sm;
  auto task = sm.start();

  // Login as admin
  Login admin_login{};
  admin_login.user_id = 1;
  admin_login.role = 1;
  sm.dispatch(admin_login);
  task.resume();

  CHECK(sm.logged_in_user == 1);
  CHECK(sm.admin_logged_in == true);

  // Logout and login as regular user
  sm.dispatch(Logout{});
  task.resume();

  Login user_login{};
  user_login.user_id = 99;
  user_login.role = 0;
  sm.dispatch(user_login);
  task.resume();

  CHECK(sm.logged_in_user == 99);
  CHECK(sm.admin_logged_in == false);
  CHECK(sm.login_count == 2);
}

TEST_CASE("Event attributes - guard uses event data to accept") {
  AccountMachine sm;
  auto task = sm.start();

  Login login{};
  login.user_id = 1;
  sm.dispatch(login);
  task.resume();

  Deposit dep{};
  dep.amount = 100;
  sm.dispatch(dep);
  task.resume();
  CHECK(sm.balance == 100);

  Transfer t{};
  t.amount = 50;
  t.destination = 7;
  sm.dispatch(t);
  task.resume();

  CHECK(sm.balance == 50);
  CHECK(sm.last_transfer_dest == 7);
  CHECK(sm.last_transfer_amount == 50);
}

TEST_CASE("Event attributes - guard uses event data to reject") {
  AccountMachine sm;
  auto task = sm.start();

  Login login{};
  login.user_id = 1;
  sm.dispatch(login);
  task.resume();

  Deposit dep{};
  dep.amount = 30;
  sm.dispatch(dep);
  task.resume();
  CHECK(sm.balance == 30);

  // Transfer exceeding balance - guard rejects
  Transfer t_over{};
  t_over.amount = 50;
  t_over.destination = 3;
  sm.dispatch(t_over);
  task.resume();

  CHECK(sm.balance == 30);
  CHECK(sm.last_transfer_dest == -1);
  CHECK(sm.last_transfer_amount == 0);

  // Transfer of zero - guard rejects (amount > 0 check)
  Transfer t_zero{};
  t_zero.amount = 0;
  t_zero.destination = 3;
  sm.dispatch(t_zero);
  task.resume();

  CHECK(sm.balance == 30);
}

TEST_CASE("Event attributes - sequential events accumulate state") {
  AccountMachine sm;
  auto task = sm.start();

  Login login{};
  login.user_id = 1;
  sm.dispatch(login);
  task.resume();

  for (int i = 1; i <= 5; ++i) {
    Deposit d{};
    d.amount = i * 10;
    sm.dispatch(d);
    task.resume();
  }
  CHECK(sm.balance == 150);  // 10+20+30+40+50

  Transfer t1{};
  t1.amount = 75;
  t1.destination = 1;
  sm.dispatch(t1);
  task.resume();
  CHECK(sm.balance == 75);

  Transfer t2{};
  t2.amount = 75;
  t2.destination = 2;
  sm.dispatch(t2);
  task.resume();
  CHECK(sm.balance == 0);
  CHECK(sm.last_transfer_dest == 2);

  // Further transfers should fail (balance=0)
  Transfer t3{};
  t3.amount = 1;
  t3.destination = 3;
  sm.dispatch(t3);
  task.resume();
  CHECK(sm.balance == 0);
  CHECK(sm.last_transfer_dest == 2);  // unchanged
}

TEST_CASE("Event attributes - guard boundary: exact balance transfer") {
  AccountMachine sm;
  auto task = sm.start();

  Login login{};
  login.user_id = 1;
  sm.dispatch(login);
  task.resume();

  Deposit dep{};
  dep.amount = 100;
  sm.dispatch(dep);
  task.resume();

  // Transfer exact balance succeeds
  Transfer t{};
  t.amount = 100;
  t.destination = 5;
  sm.dispatch(t);
  task.resume();

  CHECK(sm.balance == 0);
  CHECK(sm.last_transfer_dest == 5);
}

// --- Model 2: Entry/exit actions ---

TEST_CASE("Event attributes - typed entry action receives event payload") {
  EntryExitMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/tracker/idle");
  CHECK(sm.entry_count == 1);  // initial entry

  SetLevel lvl{};
  lvl.level = 42;
  sm.dispatch(lvl);
  task.resume();

  CHECK(sm.state() == "/tracker/active");
  CHECK(sm.level == 42);
  CHECK(sm.effect_count == 1);
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "typed_entry:42");
}

TEST_CASE("Event attributes - entry action sees different values on re-entry") {
  EntryExitMachine sm;
  auto task = sm.start();

  SetLevel lvl1{};
  lvl1.level = 10;
  sm.dispatch(lvl1);
  task.resume();
  CHECK(sm.state() == "/tracker/active");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "typed_entry:10");

  // Self-transition back to active with different level
  SetLevel lvl2{};
  lvl2.level = 20;
  sm.dispatch(lvl2);
  task.resume();
  CHECK(sm.state() == "/tracker/active");
  CHECK(sm.level == 20);
  REQUIRE(sm.log.size() == 2);
  CHECK(sm.log[1] == "typed_entry:20");
}

TEST_CASE("Event attributes - exit action fires on state change") {
  EntryExitMachine sm;
  auto task = sm.start();
  CHECK(sm.exit_count == 0);

  SetLevel lvl{};
  lvl.level = 5;
  sm.dispatch(lvl);
  task.resume();

  // Exited idle, entered active
  CHECK(sm.exit_count == 1);
  CHECK(sm.entry_count == 2);  // idle entry + active entry

  sm.dispatch(Reset{});
  task.resume();

  // Exited active, entered idle
  CHECK(sm.exit_count == 2);
  CHECK(sm.entry_count == 3);
}

// --- Model 3: Choice pseudostate ---

TEST_CASE("Event attributes - choice guard routes by event code == 1") {
  ChoiceMachine sm;
  auto task = sm.start();

  Command cmd{};
  cmd.code = 1;
  cmd.arg = 99;
  sm.dispatch(cmd);
  task.resume();

  CHECK(sm.state() == "/router/mode_a");
  CHECK(sm.last_code == 1);
  CHECK(sm.last_arg == 99);
}

TEST_CASE("Event attributes - choice guard routes by event code == 2") {
  ChoiceMachine sm;
  auto task = sm.start();

  Command cmd{};
  cmd.code = 2;
  cmd.arg = 55;
  sm.dispatch(cmd);
  task.resume();

  CHECK(sm.state() == "/router/mode_b");
  CHECK(sm.last_code == 2);
  CHECK(sm.last_arg == 55);
}

TEST_CASE("Event attributes - choice falls through to else branch") {
  ChoiceMachine sm;
  auto task = sm.start();

  Command cmd{};
  cmd.code = 999;
  cmd.arg = 0;
  sm.dispatch(cmd);
  task.resume();

  CHECK(sm.state() == "/router/error");
  CHECK(sm.last_code == 999);  // stored by incoming transition effect
  CHECK(sm.last_arg == 0);
}

// --- Model 4: Internal transitions ---

TEST_CASE("Event attributes - internal transition accumulates without state change") {
  InternalMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/counter/running");

  Advance a1{};
  a1.step = 3;
  sm.dispatch(a1);
  task.resume();
  CHECK(sm.accumulated == 3);
  CHECK(sm.effect_count == 1);
  CHECK(sm.state() == "/counter/running");

  Advance a2{};
  a2.step = 7;
  sm.dispatch(a2);
  task.resume();
  CHECK(sm.accumulated == 10);
  CHECK(sm.effect_count == 2);
  CHECK(sm.state() == "/counter/running");

  Advance a3{};
  a3.step = -5;
  sm.dispatch(a3);
  task.resume();
  CHECK(sm.accumulated == 5);
  CHECK(sm.effect_count == 3);
  CHECK(sm.state() == "/counter/running");
}

TEST_CASE("Event attributes - internal transition many rapid dispatches") {
  InternalMachine sm;
  auto task = sm.start();

  for (int i = 1; i <= 20; ++i) {
    Advance a{};
    a.step = i;
    sm.dispatch(a);
    task.resume();
  }

  CHECK(sm.accumulated == 210);  // sum 1..20
  CHECK(sm.effect_count == 20);
  CHECK(sm.state() == "/counter/running");
}

// --- Model 5: Multiple effects ---

TEST_CASE("Event attributes - multiple effects all receive same event") {
  MultiEffectMachine sm;
  auto task = sm.start();

  Configure cfg{};
  cfg.mode = 3;
  cfg.threshold = 50;
  sm.dispatch(cfg);
  task.resume();

  CHECK(sm.state() == "/multi/configured");
  CHECK(sm.mode == 3);
  CHECK(sm.threshold == 50);
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "configure:3:50");
}

TEST_CASE("Event attributes - multiple effects update on reconfigure") {
  MultiEffectMachine sm;
  auto task = sm.start();

  Configure cfg1{};
  cfg1.mode = 1;
  cfg1.threshold = 10;
  sm.dispatch(cfg1);
  task.resume();

  CHECK(sm.mode == 1);
  CHECK(sm.threshold == 10);

  Configure cfg2{};
  cfg2.mode = 2;
  cfg2.threshold = 99;
  sm.dispatch(cfg2);
  task.resume();

  CHECK(sm.mode == 2);
  CHECK(sm.threshold == 99);
  REQUIRE(sm.log.size() == 2);
  CHECK(sm.log[0] == "configure:1:10");
  CHECK(sm.log[1] == "configure:2:99");
}

// --- Model 6: Hierarchical states ---

TEST_CASE("Event attributes - child state guard uses event data") {
  HierarchyMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/hier/parent/child_a");

  // level > 0 passes guard -> transition to child_b
  SetLevel lvl{};
  lvl.level = 5;
  sm.dispatch(lvl);
  task.resume();

  CHECK(sm.state() == "/hier/parent/child_b");
  CHECK(sm.level == 5);
}

TEST_CASE("Event attributes - child state guard rejects on event data") {
  HierarchyMachine sm;
  auto task = sm.start();

  // level == 0, guard returns false, no transition
  SetLevel lvl{};
  lvl.level = 0;
  sm.dispatch(lvl);
  task.resume();

  CHECK(sm.state() == "/hier/parent/child_a");
  CHECK(sm.level == 0);
}

TEST_CASE("Event attributes - parent internal transition sees child event payload") {
  HierarchyMachine sm;
  auto task = sm.start();

  Ping p{};
  p.seq = 42;
  sm.dispatch(p);
  task.resume();

  CHECK(sm.last_ping_seq == 42);
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "parent_ping:42");
  // State unchanged - internal transition
  CHECK(sm.state() == "/hier/parent/child_a");
}

TEST_CASE("Event attributes - parent internal transition works from any child") {
  HierarchyMachine sm;
  auto task = sm.start();

  // Move to child_b
  SetLevel lvl{};
  lvl.level = 1;
  sm.dispatch(lvl);
  task.resume();
  CHECK(sm.state() == "/hier/parent/child_b");

  // Ping from child_b still handled by parent
  Ping p{};
  p.seq = 77;
  sm.dispatch(p);
  task.resume();

  CHECK(sm.last_ping_seq == 77);
  CHECK(sm.state() == "/hier/parent/child_b");
}

TEST_CASE("Event attributes - round-trip through child states preserves data") {
  HierarchyMachine sm;
  auto task = sm.start();

  // child_a -> child_b (level=10)
  SetLevel l1{};
  l1.level = 10;
  sm.dispatch(l1);
  task.resume();
  CHECK(sm.state() == "/hier/parent/child_b");
  CHECK(sm.level == 10);

  // child_b -> child_a (level=20)
  SetLevel l2{};
  l2.level = 20;
  sm.dispatch(l2);
  task.resume();
  CHECK(sm.state() == "/hier/parent/child_a");
  CHECK(sm.level == 20);

  // child_a -> child_b (level=30)
  SetLevel l3{};
  l3.level = 30;
  sm.dispatch(l3);
  task.resume();
  CHECK(sm.state() == "/hier/parent/child_b");
  CHECK(sm.level == 30);
}

// --- Model 7: Deferred events ---

TEST_CASE("Event attributes - deferred event preserves payload") {
  DeferMachine sm;
  auto task = sm.start();

  CHECK(sm.state() == "/deferrer/holding");

  // Dispatch Trigger while in holding state - gets deferred
  Trigger tr{};
  tr.value = 42;
  sm.dispatch(tr);
  task.resume();

  // Still holding - Trigger was deferred
  CHECK(sm.state() == "/deferrer/holding");
  CHECK(sm.accumulated == 0);

  // Advance causes transition to processing, deferred Trigger replays
  Advance adv{};
  adv.step = 10;
  sm.dispatch(adv);
  task.resume();

  // Advance effect + deferred Trigger effect
  CHECK(sm.accumulated == 52);  // 10 + 42
  CHECK(sm.state() == "/deferrer/done");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "trigger:42");
}

TEST_CASE("Event attributes - deferred event with different values") {
  DeferMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 100;
  sm.dispatch(tr);
  task.resume();
  CHECK(sm.state() == "/deferrer/holding");

  Advance adv{};
  adv.step = 5;
  sm.dispatch(adv);
  task.resume();

  CHECK(sm.accumulated == 105);  // 5 + 100
  CHECK(sm.state() == "/deferrer/done");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "trigger:100");
}

// --- Model 8: Multiple guards ---

TEST_CASE("Event attributes - first matching guard wins (high value)") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 200;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/mg/high");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "high");
}

TEST_CASE("Event attributes - second guard matches (low value)") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 50;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/mg/low");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "low");
}

TEST_CASE("Event attributes - else guard matches (zero value)") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 0;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/mg/zero");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "zero");
}

TEST_CASE("Event attributes - guard boundary: value == 100 takes low path") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 100;
  sm.dispatch(tr);
  task.resume();

  // value > 100 is false, value > 0 is true -> low
  CHECK(sm.state() == "/mg/low");
}

TEST_CASE("Event attributes - guard boundary: value == 101 takes high path") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = 101;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/mg/high");
}

TEST_CASE("Event attributes - negative value takes zero path") {
  MultiGuardMachine sm;
  auto task = sm.start();

  Trigger tr{};
  tr.value = -10;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/mg/zero");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "zero");
}

// --- Model 9: Wildcard ---

TEST_CASE("Event attributes - specific handler preferred over wildcard") {
  WildcardMachine sm;
  auto task = sm.start();

  Ping p{};
  p.seq = 7;
  sm.dispatch(p);
  task.resume();

  CHECK(sm.state() == "/wild/pinged");
  CHECK(sm.last_ping_seq == 7);
  CHECK(sm.log.empty());  // wildcard did not fire
}

TEST_CASE("Event attributes - wildcard catches unknown events") {
  WildcardMachine sm;
  auto task = sm.start();

  // Trigger has no specific handler -> caught by AnyEvent wildcard
  Trigger tr{};
  tr.value = 99;
  sm.dispatch(tr);
  task.resume();

  CHECK(sm.state() == "/wild/caught");
  REQUIRE(sm.log.size() == 1);
  CHECK(sm.log[0] == "wildcard");
  CHECK(sm.last_ping_seq == -1);  // ping handler never fired
}
