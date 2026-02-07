#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

// Test Signal parent-chain propagation which is used for activity cancellation.
// With native coroutines, activities run synchronously but Signal propagation
// is still relevant for activities that check their signal during execution.

struct MyInstance {
    bool activity_ran{false};
    bool signal_was_set{false};
};

// Activity that checks the signal state
void checking_activity(Signal& sig, MyInstance& inst, const AnyEvent&) {
    inst.activity_ran = true;
    inst.signal_was_set = sig.is_set();
}

constexpr auto signal_check_model = define("root",
    initial(target("/root/s1")),
    state("s1",
        activity(checking_activity)
    )
);

TEST_CASE("Signal - Parent chain propagation") {
    // Test that Signal parent-chain propagation works correctly.
    // This is the mechanism used for activity cancellation.

    Signal parent;
    Signal child;

    // Initially neither is set
    CHECK_FALSE(parent.is_set());
    CHECK_FALSE(child.is_set());

    // Connect child to parent
    child.reset(&parent);
    CHECK_FALSE(child.is_set());  // Parent not set yet

    // Setting parent should propagate to child
    parent.set();
    CHECK(parent.is_set());
    CHECK(child.is_set());  // Child sees parent's signal
}

TEST_CASE("Signal - reset clears local flag and parent link") {
    Signal parent;
    Signal child;

    parent.set();
    child.reset(&parent);
    CHECK(child.is_set());  // Inherited from parent

    // Reset child without parent: clears and breaks propagation
    child.reset(nullptr);
    CHECK_FALSE(child.is_set());  // Now independent
}

TEST_CASE("Activity receives Signal reference") {
    // Test that activities receive a Signal reference they can check.
    // With native coroutines, activities run synchronously on entry.

    struct Machine : MyInstance, HSM<signal_check_model, Machine> {};
    Machine sm;
    sm.start();

    // Activity should have run synchronously on entry
    CHECK(sm.activity_ran);
    // Signal should not be set during normal entry
    CHECK_FALSE(sm.signal_was_set);
}
