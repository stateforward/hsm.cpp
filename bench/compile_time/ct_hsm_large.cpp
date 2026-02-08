// Compile-time benchmark: HSM large (20 states, 22 transitions, deep hierarchy)
#include "hsm/hsm.hpp"

using namespace hsm;

struct E1 : Event<make_kind(1, Kind::Event)> {};
struct E2 : Event<make_kind(2, Kind::Event)> {};
struct E3 : Event<make_kind(3, Kind::Event)> {};
struct E4 : Event<make_kind(4, Kind::Event)> {};
struct E5 : Event<make_kind(5, Kind::Event)> {};

static constexpr auto model = define(
    "Large",
    state("RegionA",
          state("A1"), state("A2"), state("A3"), state("A4"), state("A5"),
          initial(target("/Large/RegionA/A1")),
          transition(on<E1>(), source("/Large/RegionA/A1"), target("/Large/RegionA/A2")),
          transition(on<E1>(), source("/Large/RegionA/A2"), target("/Large/RegionA/A3")),
          transition(on<E1>(), source("/Large/RegionA/A3"), target("/Large/RegionA/A4")),
          transition(on<E1>(), source("/Large/RegionA/A4"), target("/Large/RegionA/A5")),
          transition(on<E1>(), source("/Large/RegionA/A5"), target("/Large/RegionA/A1"))),
    state("RegionB",
          state("B1"), state("B2"), state("B3"), state("B4"), state("B5"),
          initial(target("/Large/RegionB/B1")),
          transition(on<E2>(), source("/Large/RegionB/B1"), target("/Large/RegionB/B2")),
          transition(on<E2>(), source("/Large/RegionB/B2"), target("/Large/RegionB/B3")),
          transition(on<E2>(), source("/Large/RegionB/B3"), target("/Large/RegionB/B4")),
          transition(on<E2>(), source("/Large/RegionB/B4"), target("/Large/RegionB/B5")),
          transition(on<E2>(), source("/Large/RegionB/B5"), target("/Large/RegionB/B1"))),
    state("RegionC",
          state("Outer",
                state("C1"), state("C2"), state("C3"),
                initial(target("/Large/RegionC/Outer/C1")),
                transition(on<E3>(), source("/Large/RegionC/Outer/C1"), target("/Large/RegionC/Outer/C2")),
                transition(on<E3>(), source("/Large/RegionC/Outer/C2"), target("/Large/RegionC/Outer/C3")),
                transition(on<E3>(), source("/Large/RegionC/Outer/C3"), target("/Large/RegionC/Outer/C1"))),
          state("Inner",
                state("D1"), state("D2"),
                initial(target("/Large/RegionC/Inner/D1")),
                transition(on<E4>(), source("/Large/RegionC/Inner/D1"), target("/Large/RegionC/Inner/D2")),
                transition(on<E4>(), source("/Large/RegionC/Inner/D2"), target("/Large/RegionC/Inner/D1"))),
          initial(target("/Large/RegionC/Outer")),
          transition(on<E5>(), source("/Large/RegionC/Outer"), target("/Large/RegionC/Inner")),
          transition(on<E5>(), source("/Large/RegionC/Inner"), target("/Large/RegionC/Outer"))),
    transition(on<E5>(), source("/Large/RegionA/A1"), target("/Large/RegionB/B1")),
    transition(on<E5>(), source("/Large/RegionB/B1"), target("/Large/RegionA/A1")),
    initial(target("/Large/RegionA")));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    sm.start();
    sm.process<E1>();
    sm.process<E2>();
    sm.process<E3>();
    sm.process<E4>();
    sm.process<E5>();
    return 0;
}
