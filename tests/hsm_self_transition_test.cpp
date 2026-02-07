#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

struct E_Self : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};

struct Counters {
    int entry_A = 0;
    int exit_A = 0;
    int effect_self = 0;
    
    void reset() { *this = Counters{}; }
};

struct Spy : hsm::unit_instance {
    Counters counters;
    
    void on_entry_A() { counters.entry_A++; }
    void on_exit_A() { counters.exit_A++; }
    void on_effect_self() { counters.effect_self++; }
};

static constexpr auto self_transition_model = define("Root",
    initial(target("/Root/A")),
    state("A",
        entry(&Spy::on_entry_A),
        exit(&Spy::on_exit_A),
        // Self External: Should exit A, run effect, enter A
        transition(on<E_Self>(), target("/Root/A"), effect(&Spy::on_effect_self))
    )
);

TEST_CASE("Transitions - Self (External)") {
    struct Machine : Spy, HSM<self_transition_model, Machine> {};
    Machine sm;
    auto task = sm.start();

    // Initial state should be A
    CHECK(sm.state() == "/Root/A");
    CHECK(sm.counters.entry_A == 1);
    CHECK(sm.counters.exit_A == 0);

    sm.counters.reset();
    sm.dispatch<E_Self>();
    task.resume();

    CHECK(sm.state() == "/Root/A");
    CHECK(sm.counters.effect_self == 1);
    CHECK(sm.counters.exit_A == 1);  // Must exit
    CHECK(sm.counters.entry_A == 1); // Must re-enter
}
