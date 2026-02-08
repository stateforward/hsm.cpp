# HSM Test Coverage Gaps

Gaps identified by comparing against Boost.SML and HFSM2 test suites.

---

## Tier 1: High-value gaps (would catch real bugs)

### source() combinations not yet tested

1. **source() + history target** — If a source()-routed transition targets `shallow_history("/P")`, does the history restore work correctly? The LCA calculation changes when source differs from the structurally-containing state.

2. **source() + choice pseudostate** — `source("/P/A")` routing into a `choice()` that fans out via guards. Does source resolution compose with dynamic routing?

3. **source() + completion chaining** — Transition via source() lands in a state that has a completion transition. Does the completion fire? (pipe_model tests this *without* source(), but never *with* it.)

4. **source() + defer** — Event deferred in state A, then state changes to B where it's not deferred. When the deferred event replays, does source() correctly match the *new* active state (B), not the original state (A)?

5. **source() with deep nesting** — source() specifying a grandchild (`source("/R/P/C/GC")`) where the transition is defined on a great-grandparent. Tests that ancestor-matching logic works beyond parent→child.

6. **Multiple source() transitions, same event, overlapping ancestor** — Two transitions on the same event: `source("/P/A")` and `source("/P/A/X")`. When in state `/P/A/X`, which wins? Tests priority/specificity resolution.

### Cross-feature integration missing from hsm_process_test.cpp

7. **process() + history** — No process() test drives shallow/deep history. The dispatch tests cover it, but process() has a different code path.

8. **process() + choice** — Choice pseudostate never exercised through process().

9. **process() + defer** — Deferred events are never tested through process(). Does process() correctly replay deferred events after a state change?

10. **process() + final state** — Does process() handle entering a final state and triggering parent completion?

---

## Tier 2: Robustness / edge cases

11. **Guard side-effect on source miss** — Verify the guard lambda is *never invoked* when source doesn't match. Also verify effects are skipped on source miss.

12. **source() with wildcard event (AnyEvent)** — Does `transition(source("/P/A"), target("/P/B"))` (no `on<>`) work as a completion from a specific source? What about `on<AnyEvent>()` + `source()`?

13. **Multiple process() calls triggering chained completions** — State A →(Go via source)→ B →(completion)→ C →(completion)→ D. Does the full chain execute in a single `process<Go>()` call?

14. **source() on the root state** — `source("/R")` where R is the top-level machine state. Edge case for ancestor matching.

15. **Concurrent source() transitions with conflicting guards** — Two transitions: same event, same source, different guards. Only one guard passes. Tests that the framework tries both and picks the one that passes.

---

## Tier 3: What SML/HFSM2 test that we don't

16. **Orthogonal/parallel regions** — SML tests these heavily (6 dedicated test cases). If we plan to support them, source() + orthogonal regions is critical.

17. **Thread safety** — SML has `policies_thread_safe.cpp` with 2 threads × 1000 iterations. If process() is meant to be used concurrently, we need this.

18. **Serialization round-trip** — HFSM2 tests serializing active state configuration and restoring it. Useful for save/load in game-dev.

19. **constexpr execution** — SML tests that simple state machines can be fully evaluated at compile time via `static_assert`.

20. **Scale stress** — SML tests 257 states/events, HFSM2 tests 394 states/150 regions. Our largest model is the traffic light (8 states). A model with 50+ states would stress the template machinery.

21. **Regression test directory** — Both SML and HFSM2 maintain `test/reported/` directories for bugs found by users. We should start one now, even if empty.

22. **Compile-time error tests** — SML verifies that invalid DSL usage (non-callable guard, invalid transition table) produces clear errors, not template noise.

---

## Tier 4: Nice-to-have

23. **Property-based fuzzing** — Generate random event sequences, assert invariants: active state is always valid, entry/exit are always paired, source-miss never fires effects.

24. **PSSM conformance** — The OMG publishes 103 test cases for UML state machine semantics. No library passes all of them, but tracking which we comply with would be differentiating.

25. **Dispatch return value with source()** — `dispatch_return_test.cpp` tests return values for guard rejection and deferral, but never with source()-routed transitions. Does a source-miss return "unhandled" or "no transition"?
