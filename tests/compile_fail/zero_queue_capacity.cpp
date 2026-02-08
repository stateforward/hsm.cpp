// Compile-fail test: zero queue capacity triggers static_assert.
// Expected: static_assert "capacity must be greater than zero"
#include "hsm/hsm.hpp"

using namespace hsm;

struct Evt : Event<make_kind(904, Kind::Event)> {};

constexpr auto model = define("root",
    state("A", transition(on<Evt>(), target("/root/B"))),
    state("B"));

struct ZeroQueue {
    static constexpr std::size_t capacity = 0;
};

struct SM : HSM<model, SM, hsm::Clock, ZeroQueue> {};

int main() {
    SM sm;
    sm.start();
    return 0;
}
