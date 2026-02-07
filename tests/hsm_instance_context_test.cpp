#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

struct MyContext : public Signal {
    int id = 123;
};

struct CtxAwareInstance {
    int captured_id = -1;
    int other_arg = -1;

    // Constructor that TAKES the signal
    CtxAwareInstance(MyContext& ctx, int arg) 
        : captured_id(ctx.id), other_arg(arg) {}
};

struct CtxIgnorantInstance {
    int other_arg = -1;

    // Constructor that IGNORES the signal
    CtxIgnorantInstance(int arg) 
        : other_arg(arg) {}
};

// --- Models (Global Scope) ---

constexpr auto instance_context_model = define("root", state("s1"));

// CRTP machines that decide how to use the signal in their own constructors
struct CtxAwareMachine : CtxAwareInstance,
                         HSM<instance_context_model, CtxAwareMachine> {
    using Base = HSM<instance_context_model, CtxAwareMachine>;
    using Base::Base;

    CtxAwareMachine(MyContext& ctx, int arg)
        : CtxAwareInstance(ctx, arg), Base(ctx) {}
};

struct CtxIgnorantMachine : CtxIgnorantInstance,
                            HSM<instance_context_model, CtxIgnorantMachine> {
    using Base = HSM<instance_context_model, CtxIgnorantMachine>;
    using Base::Base;

    CtxIgnorantMachine(MyContext& ctx, int arg)
        : CtxIgnorantInstance(arg), Base(ctx) {}
};

TEST_CASE("CRTP machines can choose how to use signal in constructors") {
    MyContext ctx;
    ctx.id = 999;

    SUBCASE("Machine TAKES Signal") {
        CtxAwareMachine sm(ctx, 42);

        CHECK(sm.captured_id == 999);
        CHECK(sm.other_arg == 42);
    }

    SUBCASE("Machine IGNORES Signal for its own data") {
        CtxIgnorantMachine sm(ctx, 42);

        CHECK(sm.other_arg == 42);
    }
}
