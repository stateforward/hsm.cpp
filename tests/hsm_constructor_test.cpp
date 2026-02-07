#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "hsm/hsm.hpp"

using namespace hsm;

constexpr auto model = define("ConstructorTest");

// CRTP machine used to exercise HSM base constructors
struct Machine : HSM<model, Machine> {
    using Base = HSM<model, Machine>;
    using Base::Base;
};

TEST_CASE("HSM Constructor Overloads") {
    SUBCASE("Default constructor") {
        Machine hsm;
        hsm.start();
        CHECK(hsm.state() == "/ConstructorTest");
    }
}
