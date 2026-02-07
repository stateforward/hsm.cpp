#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

struct E_Internal : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};

struct Counters {
    int entry_A = 0;
    int exit_A = 0;
    int effect_internal = 0;
    
    void reset() { *this = Counters{}; }
};

struct Spy : hsm::unit_instance {
    Counters counters;
    
    void on_entry_A() { counters.entry_A++; }
    void on_exit_A() { counters.exit_A++; }
    void on_effect_internal() { counters.effect_internal++; }
};

static constexpr auto internal_transition_model = define("Root",
    initial(target("/Root/A")),
    state("A",
        entry(&Spy::on_entry_A),
        exit(&Spy::on_exit_A),
        // Internal: Should run effect, NO exit A, NO entry A
        transition(on<E_Internal>(), effect(&Spy::on_effect_internal))
    )
);

TEST_CASE("Transitions - Internal (Local)") {
    struct Machine : Spy, HSM<internal_transition_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Initial state should be A
    CHECK(sm.state() == "/Root/A");
    CHECK(sm.counters.entry_A == 1);
    CHECK(sm.counters.exit_A == 0);

    sm.counters.reset();
    sm.dispatch<E_Internal>();
    task.resume();

    CHECK(sm.state() == "/Root/A");
    CHECK(sm.counters.effect_internal == 1);
    CHECK(sm.counters.entry_A == 0); // Internal should NOT re-enter
    CHECK(sm.counters.exit_A == 0);  // Internal should NOT exit
}
