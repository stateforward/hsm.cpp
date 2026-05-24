#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

// --- Events ---
struct E1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct E2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct Enter : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct ExitEvent : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct Back : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct DeepBack : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};

// --- Shallow History Model ---

constexpr auto shallow_model = define(
    "ShallowHistory",
    initial(target("/ShallowHistory/Outside")),
    
    state("Outside",
        transition(on<Enter>(), target("/ShallowHistory/Container")),
        transition(on<Back>(), target("/ShallowHistory/Container/history"))),
        
    state("Container",
        initial(target("/ShallowHistory/Container/Off")),
        shallow_history("history", transition(target("/ShallowHistory/Container/Off"))), // Default to Off
        
        state("Off",
            transition(on<E1>(), target("/ShallowHistory/Container/On"))),
            
        state("On",
            transition(on<E2>(), target("/ShallowHistory/Container/Off"))),
            
        transition(on<ExitEvent>(), target("/ShallowHistory/Outside"))
    )
);

// --- Deep History Model ---

constexpr auto deep_model = define(
    "DeepHistory",
    initial(target("/DeepHistory/Outside")),
    
    state("Outside",
        transition(on<Enter>(), target("/DeepHistory/Container")),
        transition(on<DeepBack>(), target("/DeepHistory/Container/history"))),
        
    state("Container",
        initial(target("/DeepHistory/Container/Level1")),
        deep_history("history", transition(target("/DeepHistory/Container/Level1"))),
        
        state("Level1",
            initial(target("/DeepHistory/Container/Level1/A")),
            state("A", transition(on<E1>(), target("/DeepHistory/Container/Level1/B"))),
            state("B", transition(on<E2>(), target("/DeepHistory/Container/Level1/A")))
        ),
        
        transition(on<ExitEvent>(), target("/DeepHistory/Outside"))
    )
);

TEST_CASE("Shallow History") {
    struct Machine : HSM<shallow_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // 1. Initial entry -> Off
    CHECK(sm.state() == "/ShallowHistory/Outside");
    sm.dispatch<Enter>();
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Container/Off");

    // 2. Transition Off -> On
    sm.dispatch<E1>();
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Container/On");

    // 3. Exit container
    sm.dispatch<ExitEvent>();
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Outside");

    // 4. Re-enter via history -> Should return to On (not initial Off)
    sm.dispatch<Back>();
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Container/On");

    // 5. Reset to Off, Exit, Re-enter
    sm.dispatch<E2>(); // On -> Off
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Container/Off");
    sm.dispatch<ExitEvent>();
    task.resume();
    sm.dispatch<Back>();
    task.resume();
    CHECK(sm.state() == "/ShallowHistory/Container/Off");
}

TEST_CASE("Deep History") {
    struct Machine : HSM<deep_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // 1. Enter -> Level1 -> A
    sm.dispatch<Enter>();
    task.resume();
    CHECK(sm.state() == "/DeepHistory/Container/Level1/A");

    // 2. A -> B
    sm.dispatch<E1>();
    task.resume();
    CHECK(sm.state() == "/DeepHistory/Container/Level1/B");

    // 3. Exit container
    sm.dispatch<ExitEvent>();
    task.resume();
    CHECK(sm.state() == "/DeepHistory/Outside");

    // 4. Re-enter via deep history -> Should restore Level1 -> B
    sm.dispatch<DeepBack>();
    task.resume();
    CHECK(sm.state() == "/DeepHistory/Container/Level1/B");
}

TEST_CASE("History Default Transition") {
    // Tests that if no history is recorded (first entry), it uses the history's default transition
    struct Machine : HSM<shallow_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Direct entry via history on first run
    sm.dispatch<Back>();
    task.resume();

    // Should go to Off (history default), avoiding potential undefined behavior
    CHECK(sm.state() == "/ShallowHistory/Container/Off");
}
