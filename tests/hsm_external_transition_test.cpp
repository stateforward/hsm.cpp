#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

struct E_External : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct E_ToParent : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};

struct Counters {
    int entry_A = 0;
    int exit_A = 0;
    int entry_B = 0;
    int exit_B = 0;
    int effect_external = 0;
    int entry_Parent = 0;
    int entry_Child = 0;
    int exit_Parent = 0;
    
    void reset() { *this = Counters{}; }
};

struct Spy : hsm::unit_instance {
    Counters counters;
    
    void on_entry_A() { counters.entry_A++; }
    void on_exit_A() { counters.exit_A++; }
    
    void on_entry_B() { counters.entry_B++; }
    void on_exit_B() { counters.exit_B++; }
    
    void on_effect_external() { counters.effect_external++; }

    void on_entry_Parent() { counters.entry_Parent++; }
    void on_exit_Parent() { counters.exit_Parent++; }
    
    void on_entry_Child() { counters.entry_Child++; }
};

static constexpr auto external_transition_model = define("Root",
    initial(target("/Root/A")),
    state("A",
        entry(&Spy::on_entry_A),
        exit(&Spy::on_exit_A),
        // External to B
        transition(on<E_External>(), target("/Root/B"), effect(&Spy::on_effect_external))
    ),
    state("B",
        entry(&Spy::on_entry_B),
        exit(&Spy::on_exit_B),
        transition(on<E_ToParent>(), target("/Root/Parent"))
    ),
    state("Parent",
        entry(&Spy::on_entry_Parent),
        exit(&Spy::on_exit_Parent),
        // Initial transition is implicitly LOCAL. Should enter Child without exiting Parent
        initial(target("/Root/Parent/Child")),
        state("Child",
            entry(&Spy::on_entry_Child)
        )
    )
);

TEST_CASE("Transitions - External") {
    struct Machine : Spy, HSM<external_transition_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Initial state should be A
    CHECK(sm.state() == "/Root/A");
    CHECK(sm.counters.entry_A == 1);

    SUBCASE("Simple External Transition") {
        sm.counters.reset();
        sm.dispatch<E_External>();
        task.resume();

        CHECK(sm.state() == "/Root/B");
        CHECK(sm.counters.effect_external == 1);
        CHECK(sm.counters.exit_A == 1);
        CHECK(sm.counters.entry_B == 1);
    }

    SUBCASE("External Transition to Composite (Initial)") {
        // Move to B first
        sm.dispatch<E_External>();
        task.resume();
        sm.counters.reset();

        // Move to Parent. Parent -> initial -> Child
        // Expectation: Exit B, Enter Parent, Enter Child.
        sm.dispatch<E_ToParent>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child");
        CHECK(sm.counters.exit_B == 1);
        CHECK(sm.counters.entry_Parent == 1);
        CHECK(sm.counters.entry_Child == 1);

        // Verify Parent was entered once and NOT exited during the initial transition drill-down
        CHECK(sm.counters.exit_Parent == 0);
    }
}
