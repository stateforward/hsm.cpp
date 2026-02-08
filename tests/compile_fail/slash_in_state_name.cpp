// Compile-fail test: slash in state name triggers constexpr_assert.
// Expected: constexpr_assert "hsm name must not contain '/'"
#include "hsm/hsm.hpp"

using namespace hsm;

constexpr auto model = define("root",
    state("A/B"));

struct SM : HSM<model, SM> {};

int main() {
    SM sm;
    sm.start();
    return 0;
}
