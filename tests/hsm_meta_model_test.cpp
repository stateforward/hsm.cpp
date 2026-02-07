#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

constexpr auto hierarchy_model = define("root",
    state("s1",
        state("s1_1"),
        state("s1_2")
    ),
    state("s2")
);

TEST_CASE("Normalization - State Hierarchy") {
    constexpr auto data = hsm::detail::normalize<hierarchy_model>();
    
    CHECK(data.state_count == 5); // root, s1, s1_1, s1_2, s2
    
    CHECK(data.get_state_name(0) == "/root");
    CHECK(data.get_state_name(1) == "/root/s1");
    CHECK(data.get_state_name(2) == "/root/s1/s1_1");
    CHECK(data.get_state_name(3) == "/root/s1/s1_2");
    CHECK(data.get_state_name(4) == "/root/s2");
    
    // Check parents
    CHECK(data.states[0].parent_id == hsm::detail::invalid_index);
    CHECK(data.states[1].parent_id == 0);
    CHECK(data.states[2].parent_id == 1);
    CHECK(data.states[3].parent_id == 1);
    CHECK(data.states[4].parent_id == 0);
}

constexpr auto transition_resolution_model = define("root",
    state("A",
        transition(target("/root/B")),      // Sibling resolution
        transition(target("/root/B")) // Absolute resolution
    ),
    state("B")
);

TEST_CASE("Normalization - Transition Resolution") {
    constexpr auto data = hsm::detail::normalize<transition_resolution_model>();
    
    CHECK(data.state_count == 3);
    CHECK(data.transition_count == 2);
    
    std::size_t id_A = 1;
    std::size_t id_B = 2;
    
    CHECK(data.get_state_name(id_A) == "/root/A");
    CHECK(data.get_state_name(id_B) == "/root/B");
    
    // Transition 0: Sibling
    CHECK(data.transitions[0].source_id == id_A);
    CHECK(data.transitions[0].target_id == id_B);
    
    // Transition 1: Absolute
    CHECK(data.transitions[1].source_id == id_A);
    CHECK(data.transitions[1].target_id == id_B);
}

struct E1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct E2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct E3 : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};

constexpr auto deferral_model = define("root", state("s1",
                                            transition(on<E1>(), target("/root/s2")),
                                            defer<E2, E3>()),
                              state("s2", transition(on<E2>())));

TEST_CASE("Normalization - Events and Deferral") {
  constexpr auto data = hsm::detail::normalize<deferral_model>();

  // Count events: E1 (in trans), E2 (in defer), E3 (in defer), E2 (in trans)
  // Order: Defer (Pass 1) -> Transitions (Pass 2)
  CHECK(data.event_count == 4);

  // Defer events from s1
  CHECK(data.events[0].kind == E2::kind);
  CHECK(data.events[1].kind == E3::kind);

  // Transition event from s1
  CHECK(data.events[2].kind == E1::kind);

  // Transition event from s2
  CHECK(data.events[3].kind == E2::kind);

  // Verify transitions link to events
  // T0: s1 -> s2 on E1 (ID 2)
  CHECK(data.transitions[0].event_id == 2);

  // T1: s2 -> internal? on E2 (ID 3)
  CHECK(data.transitions[1].event_id == 3);
}
