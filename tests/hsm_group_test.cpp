#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"
#include "hsm/group.hpp"

using namespace hsm;

// Define some simple events
struct Start : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct Stop : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};
struct Ping : hsm::Event<hsm::make_kind(12, hsm::Kind::Event)> {};

// Events for wildcard and shutdown group edge-case tests
struct WildFoo : hsm::Event<hsm::make_kind(20, hsm::Kind::Event)> {};
struct WildBar : hsm::Event<hsm::make_kind(21, hsm::Kind::Event)> {};
struct Shutdown : hsm::Event<hsm::make_kind(22, hsm::Kind::Event)> {};

// Define Machine 1
constexpr auto model1 = define(
    "Machine1", initial(target("/Machine1/idle")),
    state("idle", transition(on<Start>(), target("/Machine1/running")),
                  transition(on("ping"), target("/Machine1/pinged"))),
    state("running", transition(on<Stop>(), target("/Machine1/idle"))),
    state("pinged", transition(on<Stop>(), target("/Machine1/idle")))
);

// Define Machine 2 (Different type)
constexpr auto model2 = define(
    "Machine2", initial(target("/Machine2/ready")),
    state("ready", transition(on<Start>(), target("/Machine2/active")),
                   transition(on("ping"), target("/Machine2/notified"))),
    state("active", transition(on<Stop>(), target("/Machine2/ready"))),
    state("notified", transition(on<Stop>(), target("/Machine2/ready")))
);

// Define Machine 3 (Different type)
constexpr auto model3 = define(
    "Machine3", initial(target("/Machine3/waiting")),
    state("waiting", transition(on<Ping>(), target("/Machine3/done"))),
    state("done")
);

// Model with specific + wildcard handlers for the same machine
constexpr auto specific_wildcard_model = define(
    "SpecificWildcard", initial(target("/SpecificWildcard/idle")),
    state("idle",
          transition(on<WildFoo>(), target("/SpecificWildcard/foo")),
          transition(on<AnyEvent>(), target("/SpecificWildcard/any"))),
    state("foo"),
    state("any")
);

// Model that only has an AnyEvent wildcard handler
constexpr auto wildcard_only_model = define(
    "WildcardOnly", initial(target("/WildcardOnly/idle")),
    state("idle", transition(on<AnyEvent>(), target("/WildcardOnly/any"))),
    state("any")
);

// Simple stoppable machine used to model "shutdown"/final behavior in groups
constexpr auto stoppable_model = define(
    "Stoppable", initial(target("/Stoppable/active")),
    state("active", transition(on<Shutdown>(), target("/Stoppable/off"))),
    final("off")
);

// Helper CRTP Machine Wrappers
struct Machine1 : HSM<model1, Machine1> {
    using Base = HSM<model1, Machine1>;
    constexpr Machine1(std::string_view id) : Base(id) {}
};

struct Machine2 : HSM<model2, Machine2> {
    using Base = HSM<model2, Machine2>;
    constexpr Machine2(std::string_view id) : Base(id) {}
};

struct Machine3 : HSM<model3, Machine3> {
    using Base = HSM<model3, Machine3>;
    constexpr Machine3(std::string_view id) : Base(id) {}
};

struct WildcardSpecificMachine : HSM<specific_wildcard_model, WildcardSpecificMachine> {
    using Base = HSM<specific_wildcard_model, WildcardSpecificMachine>;
    constexpr WildcardSpecificMachine(std::string_view id) : Base(id) {}
};

struct WildcardOnlyMachine : HSM<wildcard_only_model, WildcardOnlyMachine> {
    using Base = HSM<wildcard_only_model, WildcardOnlyMachine>;
    constexpr WildcardOnlyMachine(std::string_view id) : Base(id) {}
};

struct StoppableMachine : HSM<stoppable_model, StoppableMachine> {
    using Base = HSM<stoppable_model, StoppableMachine>;
    constexpr StoppableMachine(std::string_view id) : Base(id) {}
};

static Task<> immediate_task() {
    co_return;
}

static Task<> yield_once_task() {
    co_await Yield{};
    co_return;
}

TEST_CASE("Group - Basic Usage") {
    Machine1 m1("m1");
    Machine2 m2("m2");
    auto t1 = m1.start();
    auto t2 = m2.start();

    // Create group
    auto group = make_group(m1, m2);
    group.start();

    // Initial states
    CHECK(m1.state() == "/Machine1/idle");
    CHECK(m2.state() == "/Machine2/ready");

    SUBCASE("Broadcast Typed Event") {
        group.dispatch(Start{});
        t1.resume();
        t2.resume();
        CHECK(m1.state() == "/Machine1/running");
        CHECK(m2.state() == "/Machine2/active");
    }

    SUBCASE("Broadcast String Literal Event") {
        group.dispatch<"ping">();
        t1.resume();
        t2.resume();
        CHECK(m1.state() == "/Machine1/pinged");
        CHECK(m2.state() == "/Machine2/notified");
    }

    SUBCASE("Send Typed Event to m1") {
        bool res = group.dispatch("m1", Start{});
        t1.resume();
        CHECK(res);
        CHECK(m1.state() == "/Machine1/running");
        CHECK(m2.state() == "/Machine2/ready"); // Unchanged
    }

    SUBCASE("Send Typed Event to m2") {
        bool res = group.dispatch("m2", Start{});
        t2.resume();
        CHECK(res);
        CHECK(m1.state() == "/Machine1/idle"); // Unchanged
        CHECK(m2.state() == "/Machine2/active");
    }

    SUBCASE("Send String Literal Event to m1") {
        bool res = group.dispatch<"ping">("m1");
        t1.resume();
        CHECK(res);
        CHECK(m1.state() == "/Machine1/pinged");
        CHECK(m2.state() == "/Machine2/ready"); // Unchanged
    }

    SUBCASE("Send to Unknown ID") {
        bool res = group.dispatch("unknown", Start{});
        CHECK_FALSE(res);
        CHECK(m1.state() == "/Machine1/idle");
        CHECK(m2.state() == "/Machine2/ready");
    }
}

TEST_CASE("GroupTask - Drives Group Machines") {
    Machine1 m1("m1");
    Machine2 m2("m2");

    auto group = make_group(m1, m2);
    auto gt = group.start();

    CHECK(gt.joinable());
    CHECK(m1.state() == "/Machine1/idle");
    CHECK(m2.state() == "/Machine2/ready");

    group.dispatch(Start{});
    gt.resume();

    CHECK(m1.state() == "/Machine1/running");
    CHECK(m2.state() == "/Machine2/active");
}

TEST_CASE("GroupTask - Aggregation Semantics") {
    auto gt = GroupTask{immediate_task(), yield_once_task()};

    CHECK(gt.joinable());
    CHECK_FALSE(gt.done());
    CHECK_FALSE(gt.deadline().has_value());

    gt.resume();
    CHECK(gt.joinable());
    CHECK_FALSE(gt.done());
    CHECK_FALSE(gt.deadline().has_value());

    gt.resume();
    CHECK_FALSE(gt.joinable());
    CHECK(gt.done());
}

TEST_CASE("Group - Heterogeneous Handling") {
    // Machine 3 only handles Ping, not Start

    Machine1 m1("m1");
    Machine3 m3("m3");
    auto t1 = m1.start();
    auto t3 = m3.start();

    auto group = make_group(m1, m3);
    group.start();

    SUBCASE("Broadcast event handled by some but not all") {
        // m1 handles Start, m3 does not.
        // Both receive it. m3 should ignore it gracefully.
        group.dispatch(Start{});
        t1.resume();
        t3.resume();

        CHECK(m1.state() == "/Machine1/running");
        CHECK(m3.state() == "/Machine3/waiting"); // Unchanged
    }

    SUBCASE("Broadcast event handled by other") {
        group.dispatch(Ping{});
        t1.resume();
        t3.resume();

        CHECK(m1.state() == "/Machine1/idle"); // m1 doesn't handle Ping
        CHECK(m3.state() == "/Machine3/done");
    }
}

TEST_CASE("Group - Nested Groups") {
    Machine1 m1("inner_m1");
    Machine2 m2("m2");
    auto t1 = m1.start();
    auto t2 = m2.start();

    // Inner group contains m1
    auto inner = make_group("inner", m1);
    inner.start();

    // Outer group contains inner group and m2
    auto outer = make_group(inner, m2);
    outer.start();

    SUBCASE("Recursive Broadcast") {
        // Should reach m1 (via inner) and m2
        outer.dispatch(Start{});
        t1.resume();
        t2.resume();

        CHECK(m1.state() == "/Machine1/running");
        CHECK(m2.state() == "/Machine2/active");
    }

    SUBCASE("Deep Send by ID (Typed)") {
        // Send to "inner_m1" which is inside "inner"
        bool res = outer.dispatch("inner_m1", Start{});
        t1.resume();

        CHECK(res);
        CHECK(m1.state() == "/Machine1/running");
        CHECK(m2.state() == "/Machine2/ready"); // Unchanged
    }

    SUBCASE("Deep Send by ID (String Literal)") {
        bool res = outer.dispatch<"ping">("inner_m1");
        t1.resume();

        CHECK(res);
        CHECK(m1.state() == "/Machine1/pinged");
        CHECK(m2.state() == "/Machine2/ready");
    }

    SUBCASE("Send to Inner Group itself") {
        // Sending to "inner" should broadcast to m1
        bool res = outer.dispatch("inner", Start{});
        t1.resume();

        CHECK(res);
        CHECK(m1.state() == "/Machine1/running");
    }
}

TEST_CASE("Group - Wildcard vs Specific Handlers") {
    // Machine with both specific WildFoo and AnyEvent wildcard
    WildcardSpecificMachine specific("specific");
    // Machine that only has an AnyEvent wildcard
    WildcardOnlyMachine wildcard("wildcard");
    auto t1 = specific.start();
    auto t2 = wildcard.start();

    auto group = make_group(specific, wildcard);
    group.start();

    CHECK(specific.state() == "/SpecificWildcard/idle");
    CHECK(wildcard.state() == "/WildcardOnly/idle");

    SUBCASE("Specific handlers win when present") {
        bool handled = group.dispatch(WildFoo{});
        t1.resume();
        t2.resume();
        CHECK(handled);

        // Machine with a specific transition for WildFoo should use it
        CHECK(specific.state() == "/SpecificWildcard/foo");
        // Machine with only AnyEvent should still handle via the wildcard
        CHECK(wildcard.state() == "/WildcardOnly/any");
    }

    SUBCASE("Wildcards handle events with no specific transition") {
        bool handled = group.dispatch(WildBar{});
        t1.resume();
        t2.resume();
        CHECK(handled);

        // Both machines rely on AnyEvent for WildBar
        CHECK(specific.state() == "/SpecificWildcard/any");
        CHECK(wildcard.state() == "/WildcardOnly/any");
    }
}

TEST_CASE("Dispatcher - Group runtime polymorphic dispatch") {
    Machine1 m1("m1");
    Machine2 m2("m2");
    auto t1 = m1.start();
    auto t2 = m2.start();

    auto group = make_group(m1, m2);
    group.start();
    Instance *disp = &group;

    CHECK(m1.state() == "/Machine1/idle");
    CHECK(m2.state() == "/Machine2/ready");

    Start ev;
    disp->dispatch(ev);
    t1.resume();
    t2.resume();
    CHECK(m1.state() == "/Machine1/running");
    CHECK(m2.state() == "/Machine2/active");
}

TEST_CASE("Group - Nested Group Shutdown Semantics") {
    // Two stoppable machines managed through a nested group hierarchy
    StoppableMachine s1("s1");
    StoppableMachine s2("s2");
    auto t1 = s1.start();
    auto t2 = s2.start();

    auto inner = make_group("inner", s1, s2);
    inner.start();
    auto outer = make_group(inner);
    outer.start();

    CHECK(s1.state() == "/Stoppable/active");
    CHECK(s2.state() == "/Stoppable/active");

    SUBCASE("Shutdown via inner group ID") {
        // First shutdown should transition both machines to the final state
        auto handled_first = outer.dispatch("inner", Shutdown{});
        t1.resume();
        t2.resume();
        CHECK(handled_first == hsm::Processed);
        CHECK(s1.state() == "/Stoppable/off");
        CHECK(s2.state() == "/Stoppable/off");

        // Subsequent shutdowns are processed but don't trigger transitions
        // (event is processed even though machines are already off)
        auto handled_second = outer.dispatch("inner", Shutdown{});
        t1.resume();
        t2.resume();
        CHECK(handled_second == hsm::Processed);
        CHECK(s1.state() == "/Stoppable/off");
        CHECK(s2.state() == "/Stoppable/off");
    }

    SUBCASE("Target specific machine in nested group") {
        // Shutdown a single machine by its leaf ID
        auto handled_first = outer.dispatch("s1", Shutdown{});
        t1.resume();
        CHECK(handled_first == hsm::Processed);
        CHECK(s1.state() == "/Stoppable/off");
        CHECK(s2.state() == "/Stoppable/active");

        // Further targeted dispatches to the stopped machine are still processed
        auto handled_again = outer.dispatch("s1", Shutdown{});
        t1.resume();
        CHECK(handled_again == hsm::Processed);
        CHECK(s1.state() == "/Stoppable/off");
        CHECK(s2.state() == "/Stoppable/active");

        // A broadcast still reaches the remaining active machine
        auto handled_broadcast = outer.dispatch(Shutdown{});
        t1.resume();
        t2.resume();
        CHECK(handled_broadcast == hsm::Processed);
        CHECK(s1.state() == "/Stoppable/off");
        CHECK(s2.state() == "/Stoppable/off");

        // Once all nested machines are stopped, broadcasts are still processed
        auto handled_broadcast_again = outer.dispatch(Shutdown{});
        t1.resume();
        t2.resume();
        CHECK(handled_broadcast_again == hsm::Processed);
    }
}
