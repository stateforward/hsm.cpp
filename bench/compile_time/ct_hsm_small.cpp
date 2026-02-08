// Compile-time benchmark: HSM small (2 states, 2 transitions)
#include "hsm/hsm.hpp"

using namespace hsm;

struct E1 : Event<make_kind(1, Kind::Event)> {};
struct E2 : Event<make_kind(2, Kind::Event)> {};

static constexpr auto model = define(
    "PingPong",
    state("A"),
    state("B"),
    transition(on<E1>(), source("/PingPong/A"), target("/PingPong/B")),
    transition(on<E2>(), source("/PingPong/B"), target("/PingPong/A")),
    initial(target("/PingPong/A")));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    sm.start();
    sm.process<E1>();
    sm.process<E2>();
    return 0;
}
