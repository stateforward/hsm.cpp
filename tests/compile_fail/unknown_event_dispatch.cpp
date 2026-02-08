// Compile-fail test: dispatch<T>() with an event type not in the model.
// Expected: static_assert "event type T whose kind is not present in the model"
#include "hsm/hsm.hpp"

using namespace hsm;

struct KnownEvt : Event<make_kind(901, Kind::Event)> {};
struct UnknownEvt : Event<make_kind(902, Kind::Event)> {};

constexpr auto model = define("root",
    state("A", transition(on<KnownEvt>(), target("/root/B"))),
    state("B"));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    auto task = sm.start();
    sm.dispatch(UnknownEvt{});
    return 0;
}
