#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Gap #19: Constexpr execution — static_assert validation of normalize()
//
// normalize() is consteval, so all validation here uses static_assert.
// A single doctest TEST_CASE confirms compilation succeeded.
// ============================================================================

// ---------------------------------------------------------------------------
// Events (IDs 600-609)
// ---------------------------------------------------------------------------

struct CEvt1 : Event<make_kind(600, Kind::Event)> {};
struct CEvt2 : Event<make_kind(601, Kind::Event)> {};
struct CEvt3 : Event<make_kind(602, Kind::Event)> {};

// ===========================================================================
// Model 1: Hierarchy — 6 states, test state_count, names, parent_id, depth
// ===========================================================================

constexpr auto hierarchy_ce_model = define("R",
    state("A",
        state("A1"),
        state("A2")),
    state("B",
        state("B1")));

constexpr auto hierarchy_ce_data = hsm::detail::normalize<hierarchy_ce_model>();

// state_count: R, A, A1, A2, B, B1 = 6
static_assert(hierarchy_ce_data.state_count == 6);

// State names
static_assert(hierarchy_ce_data.get_state_name(0) == "/R");
static_assert(hierarchy_ce_data.get_state_name(1) == "/R/A");
static_assert(hierarchy_ce_data.get_state_name(2) == "/R/A/A1");
static_assert(hierarchy_ce_data.get_state_name(3) == "/R/A/A2");
static_assert(hierarchy_ce_data.get_state_name(4) == "/R/B");
static_assert(hierarchy_ce_data.get_state_name(5) == "/R/B/B1");

// Parent IDs
static_assert(hierarchy_ce_data.states[0].parent_id == hsm::detail::invalid_index); // R is root
static_assert(hierarchy_ce_data.states[1].parent_id == 0); // A -> R
static_assert(hierarchy_ce_data.states[2].parent_id == 1); // A1 -> A
static_assert(hierarchy_ce_data.states[3].parent_id == 1); // A2 -> A
static_assert(hierarchy_ce_data.states[4].parent_id == 0); // B -> R
static_assert(hierarchy_ce_data.states[5].parent_id == 4); // B1 -> B

// max_depth: each state adds 1; R(+1) -> A(+1) -> A1(+1) = 3
static_assert(hierarchy_ce_data.max_depth == 3);

// ===========================================================================
// Model 2: Transitions — guards, effects, events
// ===========================================================================

constexpr auto transition_ce_model = define("T",
    initial(target("/T/S1")),
    state("S1",
        transition(on<CEvt1>(),
                   guard([](auto&&...) { return true; }),
                   effect([](auto&&...) {}),
                   target("/T/S2")),
        transition(on<CEvt2>(), target("/T/S3"))),
    state("S2",
        transition(on<CEvt3>(), target("/T/S1"))),
    state("S3"));

constexpr auto transition_ce_data = hsm::detail::normalize<transition_ce_model>();

// state_count: T, S1, S2, S3 = 4
static_assert(transition_ce_data.state_count == 4);

// transition_count: initial + 3 event transitions = 4
static_assert(transition_ce_data.transition_count == 4);

// event_count: CEvt1 + CEvt2 + CEvt3 = 3
static_assert(transition_ce_data.event_count == 3);

// Verify events have distinct kinds
static_assert(transition_ce_data.events[0].kind == CEvt1::kind);
static_assert(transition_ce_data.events[1].kind == CEvt2::kind);
static_assert(transition_ce_data.events[2].kind == CEvt3::kind);

// Transition 0 is the initial transition (has no event_id)
static_assert(transition_ce_data.transitions[0].event_id == hsm::detail::invalid_index);

// Transition 1: S1 -> S2 on CEvt1 (event_id 0), has guard
static_assert(transition_ce_data.transitions[1].source_id == 1); // S1
static_assert(transition_ce_data.transitions[1].target_id == 2); // S2
static_assert(transition_ce_data.transitions[1].event_id == 0);  // CEvt1
static_assert(transition_ce_data.transitions[1].guard_idx != hsm::detail::invalid_index);
static_assert(transition_ce_data.transitions[1].type == hsm::detail::transition_kind::external);

// Transition 2: S1 -> S3 on CEvt2 (event_id 1), no guard
static_assert(transition_ce_data.transitions[2].source_id == 1); // S1
static_assert(transition_ce_data.transitions[2].target_id == 3); // S3
static_assert(transition_ce_data.transitions[2].event_id == 1);  // CEvt2
static_assert(transition_ce_data.transitions[2].guard_idx == hsm::detail::invalid_index);

// Transition 3: S2 -> S1 on CEvt3 (event_id 2)
static_assert(transition_ce_data.transitions[3].source_id == 2); // S2
static_assert(transition_ce_data.transitions[3].target_id == 1); // S1
static_assert(transition_ce_data.transitions[3].event_id == 2);  // CEvt3

// ===========================================================================
// Model 3: History + deferral
// ===========================================================================

constexpr auto advanced_ce_model = define("H",
    state("C",
        initial(target("/H/C/X")),
        shallow_history("hist", transition(target("/H/C/X"))),
        state("X"),
        state("Y"),
        defer<CEvt3>(),
        transition(on<CEvt1>(), source("/H/C/X"), target("/H/C/Y")),
        transition(on<CEvt2>(), target("/H/Out"))),
    state("Out",
        transition(on<CEvt1>(), target("/H/C/hist"))));

constexpr auto advanced_ce_data = hsm::detail::normalize<advanced_ce_model>();

// state_count: H, C, hist, X, Y, Out = 6
static_assert(advanced_ce_data.state_count == 6);

// deferred_count: CEvt3 deferred in C = 1
static_assert(advanced_ce_data.deferred_count == 1);

// History pseudostate should have shallow history_type
static_assert(advanced_ce_data.states[2].history_type == hsm::detail::history_kind::shallow);

// The transition targeting hist should have history kind set
// Find it: Out -> C/hist on CEvt1. That's the last event transition.
// Transitions: initial(C), hist_default, CEvt1(X->Y), CEvt2(C->Out), CEvt1(Out->hist)
// The history-targeting transition should have history != none
constexpr auto find_history_transition = []() constexpr {
    for (std::size_t i = 0; i < advanced_ce_data.transition_count; ++i) {
        if (advanced_ce_data.transitions[i].history != hsm::detail::history_kind::none)
            return i;
    }
    return static_cast<std::size_t>(-1);
};
static_assert(find_history_transition() != static_cast<std::size_t>(-1),
              "expected at least one history-targeting transition");

// ===========================================================================
// Runtime TEST_CASE: confirms compilation succeeded (all static_asserts passed)
// ===========================================================================

TEST_CASE("constexpr - all static_asserts passed") {
    // If this test runs, all the above static_asserts passed at compile time.
    CHECK(hierarchy_ce_data.state_count == 6);
    CHECK(transition_ce_data.transition_count == 4);
    CHECK(advanced_ce_data.deferred_count == 1);
}
