#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Gap #20: Scale Stress Test — 56 total states (5 groups × 10 leaves + 5
// composites + 1 root). Validates normalization, startup, traversal, and
// reset for large models.
// ============================================================================

// Events (IDs 700-709)
struct Advance : Event<make_kind(700, Kind::Event)> {};
struct Reset   : Event<make_kind(701, Kind::Event)> {};

// ---------------------------------------------------------------------------
// Model: 5 groups (G0..G4), each with 10 leaf states (S0..S9).
// Within each group, Advance steps S0→S1→...→S9.
// On S9 of each group, Advance crosses to the next group's S0.
// Reset from any state returns to G0/S0.
//
// Total states = 1 (root "SC") + 5 (composites) + 50 (leaves) = 56
// ---------------------------------------------------------------------------

constexpr auto scale_model = define("SC",
    initial(target("/SC/G0")),

    // Group 0
    state("G0",
        initial(target("/SC/G0/S0")),
        state("S0"), state("S1"), state("S2"), state("S3"), state("S4"),
        state("S5"), state("S6"), state("S7"), state("S8"), state("S9"),
        transition(on<Advance>(), source("/SC/G0/S0"), target("/SC/G0/S1")),
        transition(on<Advance>(), source("/SC/G0/S1"), target("/SC/G0/S2")),
        transition(on<Advance>(), source("/SC/G0/S2"), target("/SC/G0/S3")),
        transition(on<Advance>(), source("/SC/G0/S3"), target("/SC/G0/S4")),
        transition(on<Advance>(), source("/SC/G0/S4"), target("/SC/G0/S5")),
        transition(on<Advance>(), source("/SC/G0/S5"), target("/SC/G0/S6")),
        transition(on<Advance>(), source("/SC/G0/S6"), target("/SC/G0/S7")),
        transition(on<Advance>(), source("/SC/G0/S7"), target("/SC/G0/S8")),
        transition(on<Advance>(), source("/SC/G0/S8"), target("/SC/G0/S9")),
        transition(on<Advance>(), source("/SC/G0/S9"), target("/SC/G1"))),

    // Group 1
    state("G1",
        initial(target("/SC/G1/S0")),
        state("S0"), state("S1"), state("S2"), state("S3"), state("S4"),
        state("S5"), state("S6"), state("S7"), state("S8"), state("S9"),
        transition(on<Advance>(), source("/SC/G1/S0"), target("/SC/G1/S1")),
        transition(on<Advance>(), source("/SC/G1/S1"), target("/SC/G1/S2")),
        transition(on<Advance>(), source("/SC/G1/S2"), target("/SC/G1/S3")),
        transition(on<Advance>(), source("/SC/G1/S3"), target("/SC/G1/S4")),
        transition(on<Advance>(), source("/SC/G1/S4"), target("/SC/G1/S5")),
        transition(on<Advance>(), source("/SC/G1/S5"), target("/SC/G1/S6")),
        transition(on<Advance>(), source("/SC/G1/S6"), target("/SC/G1/S7")),
        transition(on<Advance>(), source("/SC/G1/S7"), target("/SC/G1/S8")),
        transition(on<Advance>(), source("/SC/G1/S8"), target("/SC/G1/S9")),
        transition(on<Advance>(), source("/SC/G1/S9"), target("/SC/G2"))),

    // Group 2
    state("G2",
        initial(target("/SC/G2/S0")),
        state("S0"), state("S1"), state("S2"), state("S3"), state("S4"),
        state("S5"), state("S6"), state("S7"), state("S8"), state("S9"),
        transition(on<Advance>(), source("/SC/G2/S0"), target("/SC/G2/S1")),
        transition(on<Advance>(), source("/SC/G2/S1"), target("/SC/G2/S2")),
        transition(on<Advance>(), source("/SC/G2/S2"), target("/SC/G2/S3")),
        transition(on<Advance>(), source("/SC/G2/S3"), target("/SC/G2/S4")),
        transition(on<Advance>(), source("/SC/G2/S4"), target("/SC/G2/S5")),
        transition(on<Advance>(), source("/SC/G2/S5"), target("/SC/G2/S6")),
        transition(on<Advance>(), source("/SC/G2/S6"), target("/SC/G2/S7")),
        transition(on<Advance>(), source("/SC/G2/S7"), target("/SC/G2/S8")),
        transition(on<Advance>(), source("/SC/G2/S8"), target("/SC/G2/S9")),
        transition(on<Advance>(), source("/SC/G2/S9"), target("/SC/G3"))),

    // Group 3
    state("G3",
        initial(target("/SC/G3/S0")),
        state("S0"), state("S1"), state("S2"), state("S3"), state("S4"),
        state("S5"), state("S6"), state("S7"), state("S8"), state("S9"),
        transition(on<Advance>(), source("/SC/G3/S0"), target("/SC/G3/S1")),
        transition(on<Advance>(), source("/SC/G3/S1"), target("/SC/G3/S2")),
        transition(on<Advance>(), source("/SC/G3/S2"), target("/SC/G3/S3")),
        transition(on<Advance>(), source("/SC/G3/S3"), target("/SC/G3/S4")),
        transition(on<Advance>(), source("/SC/G3/S4"), target("/SC/G3/S5")),
        transition(on<Advance>(), source("/SC/G3/S5"), target("/SC/G3/S6")),
        transition(on<Advance>(), source("/SC/G3/S6"), target("/SC/G3/S7")),
        transition(on<Advance>(), source("/SC/G3/S7"), target("/SC/G3/S8")),
        transition(on<Advance>(), source("/SC/G3/S8"), target("/SC/G3/S9")),
        transition(on<Advance>(), source("/SC/G3/S9"), target("/SC/G4"))),

    // Group 4 (terminal group — no cross-group advance from S9)
    state("G4",
        initial(target("/SC/G4/S0")),
        state("S0"), state("S1"), state("S2"), state("S3"), state("S4"),
        state("S5"), state("S6"), state("S7"), state("S8"), state("S9"),
        transition(on<Advance>(), source("/SC/G4/S0"), target("/SC/G4/S1")),
        transition(on<Advance>(), source("/SC/G4/S1"), target("/SC/G4/S2")),
        transition(on<Advance>(), source("/SC/G4/S2"), target("/SC/G4/S3")),
        transition(on<Advance>(), source("/SC/G4/S3"), target("/SC/G4/S4")),
        transition(on<Advance>(), source("/SC/G4/S4"), target("/SC/G4/S5")),
        transition(on<Advance>(), source("/SC/G4/S5"), target("/SC/G4/S6")),
        transition(on<Advance>(), source("/SC/G4/S6"), target("/SC/G4/S7")),
        transition(on<Advance>(), source("/SC/G4/S7"), target("/SC/G4/S8")),
        transition(on<Advance>(), source("/SC/G4/S8"), target("/SC/G4/S9"))),

    // Reset from any state returns to initial
    transition(on<Reset>(), target("/SC/G0")));

struct ScaleSM : HSM<scale_model, ScaleSM> {};

// ---------------------------------------------------------------------------
// static_assert: normalize succeeded with expected state count
// ---------------------------------------------------------------------------

constexpr auto scale_data = hsm::detail::normalize<scale_model>();
static_assert(scale_data.state_count == 56);

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("scale - starts in initial state") {
    ScaleSM sm;
    sm.start();
    CHECK(sm.state() == "/SC/G0/S0");
}

TEST_CASE("scale - processes through all 50 leaves") {
    ScaleSM sm;
    sm.start();
    CHECK(sm.state() == "/SC/G0/S0");

    // 49 Advance events to reach G4/S9
    for (int i = 0; i < 49; ++i) {
        sm.process<Advance>();
    }
    CHECK(sm.state() == "/SC/G4/S9");
}

TEST_CASE("scale - reset from deep state") {
    ScaleSM sm;
    sm.start();

    // Advance partway (e.g., 25 steps into G2/S5)
    for (int i = 0; i < 25; ++i) {
        sm.process<Advance>();
    }
    CHECK(sm.state() == "/SC/G2/S5");

    // Reset returns to initial
    sm.process<Reset>();
    CHECK(sm.state() == "/SC/G0/S0");
}

TEST_CASE("scale - full round-trip") {
    ScaleSM sm;
    sm.start();

    // Chain through all 50 leaves
    for (int i = 0; i < 49; ++i) {
        sm.process<Advance>();
    }
    CHECK(sm.state() == "/SC/G4/S9");

    // Reset
    sm.process<Reset>();
    CHECK(sm.state() == "/SC/G0/S0");

    // Chain again
    for (int i = 0; i < 49; ++i) {
        sm.process<Advance>();
    }
    CHECK(sm.state() == "/SC/G4/S9");
}
