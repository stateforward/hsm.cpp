# PSSM Conformance Tracking

This document maps OMG PSSM (Precise Semantics of UML State Machines) behavioral
categories to this library's test coverage.

**Status key:**
- **Covered** — dedicated tests exist and pass
- **Partial** — some aspects tested, others not yet
- **Not Covered** — no tests exist
- **N/A** — not applicable to this library's scope

## Conformance Matrix

| # | PSSM Category | Status | Test File(s) |
|---|---------------|--------|--------------|
| 1 | Simple transitions | Covered | `hsm_dispatch_test.cpp`, `hsm_external_transition_test.cpp` |
| 2 | Composite states | Covered | `hsm_dispatch_test.cpp`, `hsm_depth_test.cpp`, `hsm_process_test.cpp` |
| 3 | Entry actions / ordering | Covered | `hsm_entry_test.cpp`, `hsm_process_test.cpp` (outside-in verified) |
| 4 | Exit actions / ordering | Covered | `hsm_exit_test.cpp`, `hsm_process_test.cpp` (inside-out verified) |
| 5 | History pseudostates (shallow) | Covered | `hsm_history_test.cpp`, `hsm_dispatch_test.cpp`, `hsm_process_test.cpp` |
| 6 | History pseudostates (deep) | Covered | `hsm_history_test.cpp`, `hsm_dispatch_test.cpp` |
| 7 | Completion events | Covered | `hsm_completion_event_test.cpp`, `hsm_process_test.cpp` |
| 8 | Choice pseudostates | Covered | `hsm_choice_test.cpp`, `hsm_process_test.cpp` |
| 9 | Guards | Covered | `hsm_transition_guard_test.cpp`, `hsm_process_test.cpp` |
| 10 | Effects (transition actions) | Covered | `hsm_transition_effect_test.cpp`, `hsm_process_test.cpp` |
| 11 | Internal transitions | Covered | `hsm_internal_transition_inheritance_test.cpp`, `hsm_process_test.cpp` |
| 12 | Deferred events | Covered | `hsm_defer_test.cpp`, `hsm_process_test.cpp` |
| 13 | Final states | Covered | `hsm_final_test.cpp`, `hsm_process_test.cpp` |
| 14 | Initial pseudostates | Covered | `hsm_initial_test.cpp`, `hsm_dispatch_test.cpp` |
| 15 | Self-transitions | Covered | `hsm_self_transition_test.cpp`, `hsm_process_test.cpp` |
| 16 | Local transitions | Covered | `hsm_local_transition_test.cpp` |
| 17 | Event processing priority | Partial | `hsm_process_test.cpp` (conflicting guards, specificity). No explicit priority test for event type vs wildcard ordering beyond `hsm_dispatch_test.cpp` wildcard case. |

## Notes

- **Orthogonal regions** are not supported by this library (N/A for PSSM orthogonal state tests).
- **Junction pseudostates** are not yet implemented; choice pseudostates cover the branching use case.
- **Do-activities** are covered by `hsm_activity_test.cpp` (coroutine-based activities).
- **source() DSL** extends PSSM by allowing parent-level transitions to be filtered by source state. This is tested in `hsm_process_test.cpp` and `hsm_dispatch_return_source_test.cpp`.
- **Fuzz testing** (`hsm_fuzz_test.cpp`) provides additional structural invariant verification across random event sequences.
