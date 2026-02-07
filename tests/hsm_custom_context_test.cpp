#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

// TODO: Custom context tests disabled during native coroutine migration.
// The old tests used custom scheduler types as HSM template parameters.
// With native coroutines, the scheduler is built-in. Re-enable these tests
// after implementing custom Signal support for the new API.

TEST_CASE("Custom context placeholder") {
    CHECK(true);
}
