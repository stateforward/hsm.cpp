// Compile-fail test: target path that doesn't exist in the model.
// Expected: constexpr_assert in resolve_target during consteval normalize().
#include "hsm/hsm.hpp"

using namespace hsm;

struct Evt : Event<make_kind(903, Kind::Event)> {};

constexpr auto model = define("root",
    state("A", transition(on<Evt>(), target("/root/DoesNotExist"))),
    state("B"));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    sm.start();
    return 0;
}
