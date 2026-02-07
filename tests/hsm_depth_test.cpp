#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

using namespace hsm;

// A test to verify that hierarchy depth > 16 is supported.
// We will construct a model with 20 nested states.

// Helper macros to generate nested states
// We'll build: Root -> L1 -> L2 .. -> L20

struct Reset : hsm::Event<hsm::make_kind(10, hsm::Kind::Event)> {};
struct Pop : hsm::Event<hsm::make_kind(11, hsm::Kind::Event)> {};

constexpr auto deep_machine_model = define(
    "DeepMachine",
    initial(target("/DeepMachine/L1")),
    state("L1",
      state("L2",
        state("L3",
          state("L4",
            state("L5",
              state("L6",
                state("L7",
                  state("L8",
                    state("L9",
                      state("L10",
                        state("L11",
                          state("L12",
                            state("L13",
                              state("L14",
                                state("L15",
                                  state("L16",
                                    state("L17",
                                      state("L18",
                                        state("L19",
                                          state("L20",
                                              transition(on<Reset>(), target("/DeepMachine/L1"))
                                          )
                                        )
                                      )
                                    )
                                  )
                                )
                              )
                            )
                          )
                        )
                      )
                    )
                  )
                )
              )
            )
          )
        )
      )
    )
);

constexpr auto drill_machine_model = define(
    "DrillMachine",
    initial(target("/DrillMachine/L1")),
    state("L1", initial(target("/DrillMachine/L1/L2")),
      state("L2", initial(target("/DrillMachine/L1/L2/L3")),
        state("L3", initial(target("/DrillMachine/L1/L2/L3/L4")),
          state("L4", initial(target("/DrillMachine/L1/L2/L3/L4/L5")),
            state("L5", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6")),
              state("L6", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7")),
                state("L7", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8")),
                  state("L8", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9")),
                    state("L9", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10")),
                      state("L10", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11")),
                        state("L11", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12")),
                          state("L12", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13")),
                            state("L13", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14")),
                              state("L14", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15")),
                                state("L15", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16")),
                                  state("L16", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17")),
                                    state("L17", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17/L18")),
                                      state("L18", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17/L18/L19")),
                                        state("L19", initial(target("/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17/L18/L19/L20")),
                                          state("L20",
                                              transition(on<Pop>(), target("/DrillMachine/L1"))
                                          )
                                        )
                                      )
                                    )
                                  )
                                )
                              )
                            )
                          )
                        )
                      )
                    )
                  )
                )
              )
            )
          )
        )
      )
    )
);

TEST_CASE("Deep Hierarchy Support (>16 levels)") {
  // If the hardcoded limit of 16 was still in place, this might compile (if array bounds check is runtime)
  // or fail compile (if static assert) or fail runtime logic (LCA truncation).
  // The LCA logic in tables.hpp used to break at 16, meaning it would fail to find common ancestor correctly
  // for very deep transitions.

  struct Machine : HSM<deep_machine_model, Machine> {};
  Machine sm;
  sm.start();
  // Should start at L1, but initial is not deep.
  // Wait, L1 has no initial. So it stays at L1?
  // The model says initial(target("L1")).
  // So it enters L1. L1 has substates but no initial.
  // So active state is L1.
  CHECK(sm.state() == "/DeepMachine/L1");
  
  // Let's manually drill down? No, we need a transition to L20 or initial chain.
  // Let's add initial chain to make it go deep automatically?
  // Or just dispatch to target L20?
  // Let's refine the model to auto-drill.
}

TEST_CASE("Deep Hierarchy Auto-Drill") {
  struct Machine : HSM<drill_machine_model, Machine> {};
  Machine sm;
  sm.start();

  // It should have drilled all the way down to L20
  CHECK(sm.state() == "/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17/L18/L19/L20");
  
  // Test LCA calculation for a long jump up
  sm.dispatch<Pop>();
  
  // Should be back at L1 -> L2 .. -> L20 (because of initial transitions re-entering)
  // If POP targets L1, it exits L20..L1, enters L1, then initial chain L2..L20.
  // So state should be L20 again.
  CHECK(sm.state() == "/DrillMachine/L1/L2/L3/L4/L5/L6/L7/L8/L9/L10/L11/L12/L13/L14/L15/L16/L17/L18/L19/L20");
}
