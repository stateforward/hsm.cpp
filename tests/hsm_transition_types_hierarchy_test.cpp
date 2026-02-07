#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Events for testing different transition types
// ============================================================================

// Internal transitions (no target)
struct E_InternalLeaf : Event<make_kind(1, Kind::Event)> {};
struct E_InternalComposite : Event<make_kind(2, Kind::Event)> {};

// External transitions
struct E_ToSibling : Event<make_kind(10, Kind::Event)> {};
struct E_ToA : Event<make_kind(11, Kind::Event)> {};
struct E_ToChild1 : Event<make_kind(12, Kind::Event)> {};
struct E_ToGrandChild1 : Event<make_kind(13, Kind::Event)> {};
struct E_ToGrandChild2 : Event<make_kind(14, Kind::Event)> {};
struct E_ToParent : Event<make_kind(15, Kind::Event)> {};
struct E_ToChild2 : Event<make_kind(16, Kind::Event)> {};

// Self transitions
struct E_SelfLeaf : Event<make_kind(20, Kind::Event)> {};
struct E_SelfComposite : Event<make_kind(21, Kind::Event)> {};
struct E_SelfGrandChild : Event<make_kind(22, Kind::Event)> {};

// ============================================================================
// Test infrastructure
// ============================================================================

struct Counters {
    int entry_A = 0;
    int exit_A = 0;
    int entry_Sibling = 0;
    int exit_Sibling = 0;
    int entry_Parent = 0;
    int exit_Parent = 0;
    int entry_Child1 = 0;
    int exit_Child1 = 0;
    int entry_Child2 = 0;
    int exit_Child2 = 0;
    int entry_GrandChild1 = 0;
    int exit_GrandChild1 = 0;
    int entry_GrandChild2 = 0;
    int exit_GrandChild2 = 0;

    int effect_internal_leaf = 0;
    int effect_internal_composite = 0;
    int effect_external = 0;
    int effect_self = 0;

    void reset() { *this = Counters{}; }
};

struct Spy : hsm::unit_instance {
    Counters counters;
    std::vector<std::string> log;

    void clear() {
        counters.reset();
        log.clear();
    }

    // Entry behaviors
    void on_entry_A() { counters.entry_A++; log.push_back("entry_A"); }
    void on_entry_Sibling() { counters.entry_Sibling++; log.push_back("entry_Sibling"); }
    void on_entry_Parent() { counters.entry_Parent++; log.push_back("entry_Parent"); }
    void on_entry_Child1() { counters.entry_Child1++; log.push_back("entry_Child1"); }
    void on_entry_Child2() { counters.entry_Child2++; log.push_back("entry_Child2"); }
    void on_entry_GrandChild1() { counters.entry_GrandChild1++; log.push_back("entry_GrandChild1"); }
    void on_entry_GrandChild2() { counters.entry_GrandChild2++; log.push_back("entry_GrandChild2"); }

    // Exit behaviors
    void on_exit_A() { counters.exit_A++; log.push_back("exit_A"); }
    void on_exit_Sibling() { counters.exit_Sibling++; log.push_back("exit_Sibling"); }
    void on_exit_Parent() { counters.exit_Parent++; log.push_back("exit_Parent"); }
    void on_exit_Child1() { counters.exit_Child1++; log.push_back("exit_Child1"); }
    void on_exit_Child2() { counters.exit_Child2++; log.push_back("exit_Child2"); }
    void on_exit_GrandChild1() { counters.exit_GrandChild1++; log.push_back("exit_GrandChild1"); }
    void on_exit_GrandChild2() { counters.exit_GrandChild2++; log.push_back("exit_GrandChild2"); }

    // Effects
    void on_effect_internal_leaf() { counters.effect_internal_leaf++; log.push_back("effect_internal_leaf"); }
    void on_effect_internal_composite() { counters.effect_internal_composite++; log.push_back("effect_internal_composite"); }
    void on_effect_external() { counters.effect_external++; log.push_back("effect_external"); }
    void on_effect_self() { counters.effect_self++; log.push_back("effect_self"); }
};

// ============================================================================
// Hierarchical model definition
// ============================================================================
// Structure:
// Root
// ├── A (leaf) - initial
// ├── Parent (composite)
// │   ├── Child1 (leaf) - initial
// │   └── Child2 (composite)
// │       ├── GrandChild1 (leaf) - initial
// │       └── GrandChild2 (leaf)
// └── Sibling (leaf)
// ============================================================================

static constexpr auto hierarchy_model = define("Root",
    initial(target("/Root/A")),

    // Leaf state A
    state("A",
        entry(&Spy::on_entry_A),
        exit(&Spy::on_exit_A),
        // External transitions
        transition(on<E_ToSibling>(), target("/Root/Sibling"), effect(&Spy::on_effect_external)),
        transition(on<E_ToParent>(), target("/Root/Parent")),
        transition(on<E_ToChild1>(), target("/Root/Parent/Child1")),
        transition(on<E_ToGrandChild1>(), target("/Root/Parent/Child2/GrandChild1")),
        // Self transition
        transition(on<E_SelfLeaf>(), target("/Root/A"), effect(&Spy::on_effect_self))
    ),

    // Composite state Parent
    state("Parent",
        entry(&Spy::on_entry_Parent),
        exit(&Spy::on_exit_Parent),
        initial(target("/Root/Parent/Child1")),

        // Internal transition on composite - should NOT exit children
        transition(on<E_InternalComposite>(), effect(&Spy::on_effect_internal_composite)),
        // Self transition on composite - exits children and re-enters
        transition(on<E_SelfComposite>(), target("/Root/Parent"), effect(&Spy::on_effect_self)),

        // Leaf state Child1
        state("Child1",
            entry(&Spy::on_entry_Child1),
            exit(&Spy::on_exit_Child1),
            // Internal transition
            transition(on<E_InternalLeaf>(), effect(&Spy::on_effect_internal_leaf)),
            // Self transition
            transition(on<E_SelfLeaf>(), target("/Root/Parent/Child1"), effect(&Spy::on_effect_self)),
            // External transitions
            transition(on<E_ToA>(), target("/Root/A")),
            transition(on<E_ToGrandChild1>(), target("/Root/Parent/Child2/GrandChild1")),
            transition(on<E_ToSibling>(), target("/Root/Sibling"))
        ),

        // Composite state Child2
        state("Child2",
            entry(&Spy::on_entry_Child2),
            exit(&Spy::on_exit_Child2),
            initial(target("/Root/Parent/Child2/GrandChild1")),

            // GrandChild1 - leaf
            state("GrandChild1",
                entry(&Spy::on_entry_GrandChild1),
                exit(&Spy::on_exit_GrandChild1),
                // Internal transition
                transition(on<E_InternalLeaf>(), effect(&Spy::on_effect_internal_leaf)),
                // Self transition
                transition(on<E_SelfGrandChild>(), target("/Root/Parent/Child2/GrandChild1"), effect(&Spy::on_effect_self)),
                // External transitions
                transition(on<E_ToA>(), target("/Root/A")),
                transition(on<E_ToChild1>(), target("/Root/Parent/Child1")),
                transition(on<E_ToGrandChild2>(), target("/Root/Parent/Child2/GrandChild2"))
            ),

            // GrandChild2 - leaf
            state("GrandChild2",
                entry(&Spy::on_entry_GrandChild2),
                exit(&Spy::on_exit_GrandChild2),
                transition(on<E_ToGrandChild1>(), target("/Root/Parent/Child2/GrandChild1"))
            )
        )
    ),

    // Leaf state Sibling
    state("Sibling",
        entry(&Spy::on_entry_Sibling),
        exit(&Spy::on_exit_Sibling),
        transition(on<E_ToA>(), target("/Root/A")),
        transition(on<E_ToParent>(), target("/Root/Parent")),
        transition(on<E_ToChild2>(), target("/Root/Parent/Child2"))
    )
);

struct Machine : Spy, HSM<hierarchy_model, Machine> {};

// ============================================================================
// TEST CASES
// ============================================================================

TEST_CASE("Internal transitions in hierarchy") {
    Machine sm;
    auto task = sm.start();

    SUBCASE("Internal transition in leaf state - no exit/entry") {
        // Start in A, move to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        // Internal transition - effect only, no exit/entry
        sm.dispatch<E_InternalLeaf>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.effect_internal_leaf == 1);
        CHECK(sm.counters.exit_Child1 == 0);
        CHECK(sm.counters.entry_Child1 == 0);
        CHECK(sm.counters.exit_Parent == 0);
        CHECK(sm.counters.entry_Parent == 0);

        CHECK(sm.log == std::vector<std::string>{"effect_internal_leaf"});
    }

    SUBCASE("Internal transition in composite state - child stays active") {
        // Start in A, move to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        // Internal transition on Parent - Child1 should stay active
        sm.dispatch<E_InternalComposite>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.effect_internal_composite == 1);
        CHECK(sm.counters.exit_Child1 == 0);
        CHECK(sm.counters.entry_Child1 == 0);
        CHECK(sm.counters.exit_Parent == 0);
        CHECK(sm.counters.entry_Parent == 0);

        CHECK(sm.log == std::vector<std::string>{"effect_internal_composite"});
    }

    SUBCASE("Internal transition in deeply nested state") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");

        // Internal transition
        sm.dispatch<E_InternalLeaf>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");
        CHECK(sm.counters.effect_internal_leaf == 1);
        CHECK(sm.counters.exit_GrandChild1 == 0);
        CHECK(sm.counters.entry_GrandChild1 == 0);
        CHECK(sm.counters.exit_Child2 == 0);
        CHECK(sm.counters.exit_Parent == 0);
    }
}

TEST_CASE("External transitions in hierarchy") {
    Machine sm;
    auto task = sm.start();

    SUBCASE("Sibling-to-sibling at same level (A -> Sibling)") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        sm.dispatch<E_ToSibling>();
        task.resume();

        CHECK(sm.state() == "/Root/Sibling");
        CHECK(sm.counters.exit_A == 1);
        CHECK(sm.counters.effect_external == 1);
        CHECK(sm.counters.entry_Sibling == 1);

        CHECK(sm.log == std::vector<std::string>{"exit_A", "effect_external", "entry_Sibling"});
    }

    SUBCASE("Shallow to deep (A -> GrandChild1) - full entry chain") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        sm.dispatch<E_ToGrandChild1>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");
        CHECK(sm.counters.exit_A == 1);
        CHECK(sm.counters.entry_Parent == 1);
        CHECK(sm.counters.entry_Child2 == 1);
        CHECK(sm.counters.entry_GrandChild1 == 1);

        // Entry order: parent before child
        CHECK(sm.log == std::vector<std::string>{
            "exit_A", "entry_Parent", "entry_Child2", "entry_GrandChild1"
        });
    }

    SUBCASE("Deep to shallow (GrandChild1 -> A) - full exit chain") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");

        sm.dispatch<E_ToA>();
        task.resume();

        CHECK(sm.state() == "/Root/A");
        CHECK(sm.counters.exit_GrandChild1 == 1);
        CHECK(sm.counters.exit_Child2 == 1);
        CHECK(sm.counters.exit_Parent == 1);
        CHECK(sm.counters.entry_A == 1);

        // Exit order: child before parent
        CHECK(sm.log == std::vector<std::string>{
            "exit_GrandChild1", "exit_Child2", "exit_Parent", "entry_A"
        });
    }

    SUBCASE("Cross-branch within composite (Child1 -> GrandChild1)") {
        // Navigate to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        sm.dispatch<E_ToGrandChild1>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");
        CHECK(sm.counters.exit_Child1 == 1);
        CHECK(sm.counters.exit_Parent == 0);  // LCA is Parent, not exited
        CHECK(sm.counters.entry_Child2 == 1);
        CHECK(sm.counters.entry_GrandChild1 == 1);

        CHECK(sm.log == std::vector<std::string>{
            "exit_Child1", "entry_Child2", "entry_GrandChild1"
        });
    }

    SUBCASE("Cross-branch to different subtree (Child1 -> Sibling)") {
        // Navigate to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        sm.dispatch<E_ToSibling>();
        task.resume();

        CHECK(sm.state() == "/Root/Sibling");
        CHECK(sm.counters.exit_Child1 == 1);
        CHECK(sm.counters.exit_Parent == 1);  // LCA is Root, Parent exited
        CHECK(sm.counters.entry_Sibling == 1);

        CHECK(sm.log == std::vector<std::string>{
            "exit_Child1", "exit_Parent", "entry_Sibling"
        });
    }

    SUBCASE("Sibling within nested composite (GrandChild1 -> GrandChild2)") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");

        sm.dispatch<E_ToGrandChild2>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild2");
        CHECK(sm.counters.exit_GrandChild1 == 1);
        CHECK(sm.counters.exit_Child2 == 0);  // LCA is Child2, not exited
        CHECK(sm.counters.exit_Parent == 0);
        CHECK(sm.counters.entry_GrandChild2 == 1);

        CHECK(sm.log == std::vector<std::string>{
            "exit_GrandChild1", "entry_GrandChild2"
        });
    }

    SUBCASE("Deep cross-branch (GrandChild1 -> Child1)") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");

        sm.dispatch<E_ToChild1>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.exit_GrandChild1 == 1);
        CHECK(sm.counters.exit_Child2 == 1);
        CHECK(sm.counters.exit_Parent == 0);  // LCA is Parent
        CHECK(sm.counters.entry_Child1 == 1);

        CHECK(sm.log == std::vector<std::string>{
            "exit_GrandChild1", "exit_Child2", "entry_Child1"
        });
    }
}

TEST_CASE("Self transitions in hierarchy") {
    Machine sm;
    auto task = sm.start();

    SUBCASE("Self on leaf state - exits and re-enters") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        sm.dispatch<E_SelfLeaf>();
        task.resume();

        CHECK(sm.state() == "/Root/A");
        CHECK(sm.counters.exit_A == 1);
        CHECK(sm.counters.effect_self == 1);
        CHECK(sm.counters.entry_A == 1);

        CHECK(sm.log == std::vector<std::string>{"exit_A", "effect_self", "entry_A"});
    }

    SUBCASE("Self on nested leaf state") {
        // Navigate to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        sm.dispatch<E_SelfLeaf>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.exit_Child1 == 1);
        CHECK(sm.counters.exit_Parent == 0);  // Parent NOT exited (LCA is Parent)
        CHECK(sm.counters.effect_self == 1);
        CHECK(sm.counters.entry_Child1 == 1);
        CHECK(sm.counters.entry_Parent == 0);  // Parent NOT re-entered

        CHECK(sm.log == std::vector<std::string>{"exit_Child1", "effect_self", "entry_Child1"});
    }

    SUBCASE("Self on composite state - exits active substates first (UML 2.5.1)") {
        // Navigate to Child1
        sm.dispatch<E_ToChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child1");

        sm.dispatch<E_SelfComposite>();
        task.resume();

        // Self on Parent: LCA is Root (parent of Parent)
        // UML 2.5.1 Section 14.2.3.8.2: Group transitions from composite states
        // exit all substates first, starting with innermost active state.
        // Child1 exits first, then Parent exits, effect runs, Parent enters,
        // initial drills to Child1.
        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.exit_Child1 == 1);  // UML 2.5.1: Child1 exits first
        CHECK(sm.counters.exit_Parent == 1);
        CHECK(sm.counters.effect_self == 1);
        CHECK(sm.counters.entry_Parent == 1);
        CHECK(sm.counters.entry_Child1 == 1);

        CHECK(sm.log == std::vector<std::string>{
            "exit_Child1", "exit_Parent", "effect_self", "entry_Parent", "entry_Child1"
        });
    }

    SUBCASE("Self on deeply nested leaf (GrandChild1)") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");

        sm.dispatch<E_SelfGrandChild>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");
        CHECK(sm.counters.exit_GrandChild1 == 1);
        CHECK(sm.counters.exit_Child2 == 0);  // LCA is Child2, not exited
        CHECK(sm.counters.exit_Parent == 0);
        CHECK(sm.counters.effect_self == 1);
        CHECK(sm.counters.entry_GrandChild1 == 1);
        CHECK(sm.counters.entry_Child2 == 0);

        CHECK(sm.log == std::vector<std::string>{
            "exit_GrandChild1", "effect_self", "entry_GrandChild1"
        });
    }
}

TEST_CASE("Local transitions (initial semantics)") {
    Machine sm;
    auto task = sm.start();

    SUBCASE("Transition to composite - local initial drills down") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        // Transition to Parent - should enter Parent, then local initial to Child1
        sm.dispatch<E_ToParent>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child1");
        CHECK(sm.counters.exit_A == 1);
        CHECK(sm.counters.entry_Parent == 1);
        CHECK(sm.counters.entry_Child1 == 1);
        // Parent should NOT be exited during initial drill-down
        CHECK(sm.counters.exit_Parent == 0);

        CHECK(sm.log == std::vector<std::string>{
            "exit_A", "entry_Parent", "entry_Child1"
        });
    }

    SUBCASE("Transition to nested composite - multi-level local initial") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        // Transition from A directly to Child2
        sm.dispatch<E_ToGrandChild1>();  // Go to GrandChild1 first
        task.resume();
        sm.dispatch<E_ToA>();  // Back to A
        task.resume();
        sm.clear();

        // Now from Sibling to Child2 (nested composite)
        sm.dispatch<E_ToSibling>();
        task.resume();
        sm.clear();

        sm.dispatch<E_ToChild2>();
        task.resume();

        CHECK(sm.state() == "/Root/Parent/Child2/GrandChild1");
        CHECK(sm.counters.exit_Sibling == 1);
        CHECK(sm.counters.entry_Parent == 1);
        CHECK(sm.counters.entry_Child2 == 1);
        CHECK(sm.counters.entry_GrandChild1 == 1);
        // Neither Parent nor Child2 exited during drill-down
        CHECK(sm.counters.exit_Parent == 0);
        CHECK(sm.counters.exit_Child2 == 0);

        CHECK(sm.log == std::vector<std::string>{
            "exit_Sibling", "entry_Parent", "entry_Child2", "entry_GrandChild1"
        });
    }

    SUBCASE("Initial transition on startup") {
        // Fresh machine starts at A via initial
        Machine fresh_sm;
        [[maybe_unused]] auto fresh_task = fresh_sm.start();

        CHECK(fresh_sm.state() == "/Root/A");
        CHECK(fresh_sm.counters.entry_A == 1);
        CHECK(fresh_sm.counters.exit_A == 0);

        CHECK(fresh_sm.log == std::vector<std::string>{"entry_A"});
    }
}

TEST_CASE("Entry/Exit ordering in hierarchy") {
    Machine sm;
    auto task = sm.start();

    SUBCASE("Multi-level exit order: child before parent") {
        // Navigate to deepest state
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        // Exit to top level
        sm.dispatch<E_ToA>();
        task.resume();

        // Verify exit order
        auto exit_pos = [&](const std::string& name) {
            auto it = std::find(sm.log.begin(), sm.log.end(), name);
            return it != sm.log.end() ? std::distance(sm.log.begin(), it) : -1;
        };

        auto gc1_exit = exit_pos("exit_GrandChild1");
        auto c2_exit = exit_pos("exit_Child2");
        auto p_exit = exit_pos("exit_Parent");
        auto a_entry = exit_pos("entry_A");

        CHECK(gc1_exit < c2_exit);  // GrandChild1 exits before Child2
        CHECK(c2_exit < p_exit);    // Child2 exits before Parent
        CHECK(p_exit < a_entry);    // All exits before entry
    }

    SUBCASE("Multi-level entry order: parent before child") {
        CHECK(sm.state() == "/Root/A");
        sm.clear();

        // Enter to deepest state
        sm.dispatch<E_ToGrandChild1>();
        task.resume();

        auto entry_pos = [&](const std::string& name) {
            auto it = std::find(sm.log.begin(), sm.log.end(), name);
            return it != sm.log.end() ? std::distance(sm.log.begin(), it) : -1;
        };

        auto a_exit = entry_pos("exit_A");
        auto p_entry = entry_pos("entry_Parent");
        auto c2_entry = entry_pos("entry_Child2");
        auto gc1_entry = entry_pos("entry_GrandChild1");

        CHECK(a_exit < p_entry);     // Exit before any entry
        CHECK(p_entry < c2_entry);   // Parent enters before Child2
        CHECK(c2_entry < gc1_entry); // Child2 enters before GrandChild1
    }

    SUBCASE("Complete traversal order verification") {
        // Navigate to GrandChild1
        sm.dispatch<E_ToGrandChild1>();
        task.resume();
        sm.clear();

        // Cross to Child1 - partial traversal
        sm.dispatch<E_ToChild1>();
        task.resume();

        // Expected: exit_GrandChild1, exit_Child2, entry_Child1
        CHECK(sm.log.size() == 3);
        CHECK(sm.log[0] == "exit_GrandChild1");
        CHECK(sm.log[1] == "exit_Child2");
        CHECK(sm.log[2] == "entry_Child1");
    }
}
