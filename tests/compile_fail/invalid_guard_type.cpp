// Compile-fail test: guard() requires a callable, not an int.
// Expected: compilation error (template constraint failure).
#include "hsm/hsm.hpp"

using namespace hsm;

struct BadEvt : Event<make_kind(900, Kind::Event)> {};

// guard(42) should fail the requires constraint on guard()
constexpr auto bad_model = define("root",
    state("A",
        transition(on<BadEvt>(), guard(42), target("/root/B"))),
    state("B"));

struct BadSM : HSM<bad_model, BadSM> {};

int main() {
    BadSM sm;
    sm.start();
    return 0;
}
