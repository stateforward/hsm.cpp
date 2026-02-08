// Compile-time benchmark: HSM medium (traffic light - 10 states, 6 transitions, hierarchy)
#include "hsm/hsm.hpp"

using namespace hsm;

struct T : Event<make_kind(1, Kind::Event)> {};

static constexpr auto model = define(
    "Traffic",
    state("Operational",
          initial(target("/Traffic/Operational/NS")),
          state("NS",
                state("Green"),
                state("Yellow"),
                initial(target("/Traffic/Operational/NS/Green"))),
          state("EW",
                state("Green"),
                state("Yellow"),
                initial(target("/Traffic/Operational/EW/Green"))),
          state("AllRed1"),
          state("AllRed2"),
          transition(on<T>(), source("/Traffic/Operational/NS/Green"),
                     target("/Traffic/Operational/NS/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/NS/Yellow"),
                     target("/Traffic/Operational/AllRed1")),
          transition(on<T>(), source("/Traffic/Operational/AllRed1"),
                     target("/Traffic/Operational/EW/Green")),
          transition(on<T>(), source("/Traffic/Operational/EW/Green"),
                     target("/Traffic/Operational/EW/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/EW/Yellow"),
                     target("/Traffic/Operational/AllRed2")),
          transition(on<T>(), source("/Traffic/Operational/AllRed2"),
                     target("/Traffic/Operational/NS/Green"))),
    initial(target("/Traffic/Operational")));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    sm.start();
    sm.process<T>();
    sm.process<T>();
    sm.process<T>();
    sm.process<T>();
    return 0;
}
