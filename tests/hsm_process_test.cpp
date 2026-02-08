#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <numeric>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Robust tests for HSM::process() — synchronous inline dispatch.
//
// These tests verify that process() performs real state transitions, fires
// entry/exit actions, evaluates guards, executes effects, handles completion
// transitions, and produces results identical to dispatch()+resume().
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

struct Go : Event<make_kind(400, Kind::Event)> {};
struct Stop : Event<make_kind(401, Kind::Event)> {};
struct Tick : Event<make_kind(402, Kind::Event)> {};
struct DataEvt : Event<make_kind(403, Kind::Event)> {
  int payload{};
};
struct Toggle : Event<make_kind(404, Kind::Event)> {};
struct Reset : Event<make_kind(405, Kind::Event)> {};

// ---------------------------------------------------------------------------
// 1. Simple two-state model with entry/exit tracing
// ---------------------------------------------------------------------------

struct TwoStateCtx {
  std::vector<std::string> trace;
  int entry_count{0};
  int exit_count{0};
};

constexpr auto two_state_model = define(
    "TS",
    initial(target("/TS/Off")),
    state("Off",
          entry([](TwoStateCtx& c) {
            c.trace.push_back("enter:Off");
            ++c.entry_count;
          }),
          exit([](TwoStateCtx& c) {
            c.trace.push_back("exit:Off");
            ++c.exit_count;
          }),
          transition(on<Go>(), target("/TS/On"))),
    state("On",
          entry([](TwoStateCtx& c) {
            c.trace.push_back("enter:On");
            ++c.entry_count;
          }),
          exit([](TwoStateCtx& c) {
            c.trace.push_back("exit:On");
            ++c.exit_count;
          }),
          transition(on<Stop>(), target("/TS/Off"))));

struct TwoStateSM : TwoStateCtx, HSM<two_state_model, TwoStateSM> {};

// ---------------------------------------------------------------------------
// 2. Pipeline model (reuses the manual drive Controller for parity checks)
// ---------------------------------------------------------------------------

struct PipeCtx {
  std::vector<std::string> trace;
  std::vector<int> buffer;
  int processed_sum{0};
};

constexpr auto pipe_model = define(
    "pipe",
    initial(target("/pipe/off")),
    state("off",
          entry([](PipeCtx& c) { c.trace.push_back("enter:off"); }),
          exit([](PipeCtx& c) { c.trace.push_back("exit:off"); }),
          transition(on<Go>(), target("/pipe/idle"))),
    state("idle",
          entry([](PipeCtx& c) { c.trace.push_back("enter:idle"); }),
          exit([](PipeCtx& c) { c.trace.push_back("exit:idle"); }),
          // Internal transition: buffer input without leaving state
          transition(on<DataEvt>(),
                     effect([](PipeCtx& c, const DataEvt& e) {
                       c.buffer.push_back(e.payload);
                       c.trace.push_back("data:" + std::to_string(e.payload));
                     })),
          transition(on<Tick>(),
                     guard([](PipeCtx& c) { return !c.buffer.empty(); }),
                     target("/pipe/busy")),
          transition(on<Reset>(),
                     effect([](PipeCtx& c) {
                       c.buffer.clear();
                       c.trace.push_back("reset");
                     }),
                     target("/pipe/idle")),
          transition(on<Stop>(), target("/pipe/off"))),
    state("busy",
          entry([](PipeCtx& c) {
            int sum = 0;
            for (auto v : c.buffer) sum += v;
            c.processed_sum += sum;
            c.buffer.clear();
            c.trace.push_back("processed:" + std::to_string(sum));
          }),
          transition(on<Stop>(), target("/pipe/off")),
          // Completion transition: auto-return to idle
          transition(target("/pipe/idle"))));

struct PipeSM : PipeCtx, HSM<pipe_model, PipeSM> {};

// ---------------------------------------------------------------------------
// 3. Guarded model: guard toggles flag, transition only on true
// ---------------------------------------------------------------------------

struct GuardCtx {
  bool flag{false};
  int transition_count{0};
};

constexpr auto guard_model = define(
    "GM",
    initial(target("/GM/S")),
    state("S",
          transition(on<Toggle>(),
                     guard([](GuardCtx& c, const Toggle&) {
                       c.flag = !c.flag;
                       return c.flag;
                     }),
                     effect([](GuardCtx& c) { ++c.transition_count; }),
                     target("/GM/S"))));

struct GuardSM : GuardCtx, HSM<guard_model, GuardSM> {};

// ---------------------------------------------------------------------------
// 4. Hierarchical model with deep entry/exit tracing
// ---------------------------------------------------------------------------

struct HierCtx {
  std::vector<std::string> trace;
};

constexpr auto hier_model = define(
    "H",
    initial(target("/H/P")),
    state("P",
          entry([](HierCtx& c) { c.trace.push_back("enter:P"); }),
          exit([](HierCtx& c) { c.trace.push_back("exit:P"); }),
          state("C1",
                entry([](HierCtx& c) { c.trace.push_back("enter:C1"); }),
                exit([](HierCtx& c) { c.trace.push_back("exit:C1"); })),
          state("C2",
                entry([](HierCtx& c) { c.trace.push_back("enter:C2"); }),
                exit([](HierCtx& c) { c.trace.push_back("exit:C2"); })),
          initial(target("/H/P/C1")),
          transition(on<Go>(), source("/H/P/C1"), target("/H/P/C2")),
          transition(on<Stop>(), source("/H/P/C2"), target("/H/P/C1"))));

struct HierSM : HierCtx, HSM<hier_model, HierSM> {};

// ---------------------------------------------------------------------------
// 5. Traffic light ring (mirrors the benchmark model but with real tracing)
// ---------------------------------------------------------------------------

struct TrafficCtx {
  std::vector<std::string> visited;
};

// Transitions use source() DSL at the Op parent level.
constexpr auto traffic_model = define(
    "TL",
    initial(target("/TL/Op")),
    state("Op",
          initial(target("/TL/Op/NS")),
          state("NS",
                state("Green",
                      entry([](TrafficCtx& c) { c.visited.push_back("NS_Green"); })),
                state("Yellow",
                      entry([](TrafficCtx& c) { c.visited.push_back("NS_Yellow"); })),
                initial(target("/TL/Op/NS/Green"))),
          state("EW",
                state("Green",
                      entry([](TrafficCtx& c) { c.visited.push_back("EW_Green"); })),
                state("Yellow",
                      entry([](TrafficCtx& c) { c.visited.push_back("EW_Yellow"); })),
                initial(target("/TL/Op/EW/Green"))),
          state("AllRed1",
                entry([](TrafficCtx& c) { c.visited.push_back("AllRed1"); })),
          state("AllRed2",
                entry([](TrafficCtx& c) { c.visited.push_back("AllRed2"); })),
          transition(on<Tick>(), source("/TL/Op/NS/Green"),
                     target("/TL/Op/NS/Yellow")),
          transition(on<Tick>(), source("/TL/Op/NS/Yellow"),
                     target("/TL/Op/AllRed1")),
          transition(on<Tick>(), source("/TL/Op/AllRed1"),
                     target("/TL/Op/EW/Green")),
          transition(on<Tick>(), source("/TL/Op/EW/Green"),
                     target("/TL/Op/EW/Yellow")),
          transition(on<Tick>(), source("/TL/Op/EW/Yellow"),
                     target("/TL/Op/AllRed2")),
          transition(on<Tick>(), source("/TL/Op/AllRed2"),
                     target("/TL/Op/NS/Green"))));

struct TrafficSM : TrafficCtx, HSM<traffic_model, TrafficSM> {};

// ---------------------------------------------------------------------------
// 6. Three-state counter model for bulk iteration tests
// ---------------------------------------------------------------------------

struct CounterCtx {
  int transitions{0};
  int a_entries{0};
  int b_entries{0};
  int c_entries{0};
};

constexpr auto counter_model = define(
    "C",
    initial(target("/C/A")),
    state("A",
          entry([](CounterCtx& c) { ++c.a_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/B"))),
    state("B",
          entry([](CounterCtx& c) { ++c.b_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/C"))),
    state("C",
          entry([](CounterCtx& c) { ++c.c_entries; }),
          transition(on<Tick>(),
                     effect([](CounterCtx& c) { ++c.transitions; }),
                     target("/C/A"))));

struct CounterSM : CounterCtx, HSM<counter_model, CounterSM> {};

// ---------------------------------------------------------------------------
// 7. source() + guard model
// ---------------------------------------------------------------------------

struct SourceGuardCtx {
  bool allow{false};
  int guard_eval_count{0};
};

constexpr auto source_guard_model = define(
    "SG",
    initial(target("/SG/P")),
    state("P",
          initial(target("/SG/P/A")),
          state("A"),
          state("B"),
          transition(on<Go>(), source("/SG/P/A"),
                     guard([](SourceGuardCtx& c) {
                       ++c.guard_eval_count;
                       return c.allow;
                     }),
                     target("/SG/P/B")),
          transition(on<Stop>(), source("/SG/P/B"),
                     target("/SG/P/A"))));

struct SourceGuardSM : SourceGuardCtx, HSM<source_guard_model, SourceGuardSM> {};

// ---------------------------------------------------------------------------
// 8. source() + effect model
// ---------------------------------------------------------------------------

struct SourceEffectCtx {
  std::string last_effect;
  int effect_count{0};
};

constexpr auto source_effect_model = define(
    "SE",
    initial(target("/SE/P")),
    state("P",
          initial(target("/SE/P/A")),
          state("A"),
          state("B"),
          transition(on<Go>(), source("/SE/P/A"),
                     effect([](SourceEffectCtx& c) {
                       c.last_effect = "A->B";
                       ++c.effect_count;
                     }),
                     target("/SE/P/B")),
          transition(on<Go>(), source("/SE/P/B"),
                     effect([](SourceEffectCtx& c) {
                       c.last_effect = "B->A";
                       ++c.effect_count;
                     }),
                     target("/SE/P/A"))));

struct SourceEffectSM : SourceEffectCtx, HSM<source_effect_model, SourceEffectSM> {};

// ---------------------------------------------------------------------------
// 9. source() self-transition model
// ---------------------------------------------------------------------------

struct SourceSelfCtx {
  int entry_count{0};
};

constexpr auto source_self_model = define(
    "SS",
    initial(target("/SS/P")),
    state("P",
          initial(target("/SS/P/A")),
          state("A",
                entry([](SourceSelfCtx& c) { ++c.entry_count; })),
          state("B"),
          transition(on<Tick>(), source("/SS/P/A"),
                     target("/SS/P/A")),
          transition(on<Go>(), source("/SS/P/A"),
                     target("/SS/P/B"))));

struct SourceSelfSM : SourceSelfCtx, HSM<source_self_model, SourceSelfSM> {};

// ---------------------------------------------------------------------------
// 10. source() internal transition model (no target)
// ---------------------------------------------------------------------------

struct SourceInternalCtx {
  int action_count{0};
  int entry_count{0};
};

constexpr auto source_internal_model = define(
    "SI",
    initial(target("/SI/P")),
    state("P",
          initial(target("/SI/P/A")),
          state("A",
                entry([](SourceInternalCtx& c) { ++c.entry_count; })),
          state("B",
                entry([](SourceInternalCtx& c) { ++c.entry_count; })),
          transition(on<Tick>(), source("/SI/P/A"),
                     effect([](SourceInternalCtx& c) { ++c.action_count; })),
          transition(on<Go>(), source("/SI/P/A"),
                     target("/SI/P/B"))));

struct SourceInternalSM : SourceInternalCtx, HSM<source_internal_model, SourceInternalSM> {};

// ---------------------------------------------------------------------------
// 11. Mixed structural + source() transitions model
// ---------------------------------------------------------------------------

struct SourceMixedCtx {
  std::vector<std::string> trace;
};

constexpr auto source_mixed_model = define(
    "SM",
    initial(target("/SM/P")),
    state("P",
          initial(target("/SM/P/A")),
          state("A",
                entry([](SourceMixedCtx& c) { c.trace.push_back("enter:A"); }),
                // Structural transition inside the state
                transition(on<Tick>(), target("/SM/P/B"))),
          state("B",
                entry([](SourceMixedCtx& c) { c.trace.push_back("enter:B"); })),
          state("C",
                entry([](SourceMixedCtx& c) { c.trace.push_back("enter:C"); })),
          // source()-based transitions at parent level
          transition(on<Go>(), source("/SM/P/B"),
                     target("/SM/P/C")),
          transition(on<Stop>(), source("/SM/P/C"),
                     target("/SM/P/A"))));

struct SourceMixedSM : SourceMixedCtx, HSM<source_mixed_model, SourceMixedSM> {};

// ---------------------------------------------------------------------------
// 13. source() entry/exit ordering model
// ---------------------------------------------------------------------------

struct SourceEntryExitCtx {
  std::vector<std::string> trace;
};

constexpr auto source_entry_exit_model = define(
    "EO",
    initial(target("/EO/P")),
    state("P",
          entry([](SourceEntryExitCtx& c) { c.trace.push_back("enter:P"); }),
          exit([](SourceEntryExitCtx& c) { c.trace.push_back("exit:P"); }),
          initial(target("/EO/P/A")),
          state("A",
                entry([](SourceEntryExitCtx& c) { c.trace.push_back("enter:A"); }),
                exit([](SourceEntryExitCtx& c) { c.trace.push_back("exit:A"); })),
          state("B",
                entry([](SourceEntryExitCtx& c) { c.trace.push_back("enter:B"); }),
                exit([](SourceEntryExitCtx& c) { c.trace.push_back("exit:B"); })),
          transition(on<Go>(), source("/EO/P/A"),
                     target("/EO/P/B")),
          transition(on<Stop>(), source("/EO/P/B"),
                     target("/EO/P/A"))));

struct SourceEntryExitSM : SourceEntryExitCtx, HSM<source_entry_exit_model, SourceEntryExitSM> {};

// ---------------------------------------------------------------------------
// 14. source() + shallow history target model (Gap 1)
// ---------------------------------------------------------------------------

struct SourceHistCtx {
  std::vector<std::string> trace;
};

constexpr auto source_hist_model = define(
    "T1H",
    initial(target("/T1H/R")),
    state("R",
          initial(target("/T1H/R/P")),
          state("P",
                initial(target("/T1H/R/P/A")),
                shallow_history("hist", transition(target("/T1H/R/P/A"))),
                state("A",
                      entry([](SourceHistCtx& c) { c.trace.push_back("enter:A"); })),
                state("B",
                      entry([](SourceHistCtx& c) { c.trace.push_back("enter:B"); })),
                // source()-routed: Go from A→B
                transition(on<Go>(), source("/T1H/R/P/A"), target("/T1H/R/P/B")),
                // Exit P
                transition(on<Stop>(), target("/T1H/R/Out"))),
          state("Out"),
          // source()-routed re-entry targeting history
          transition(on<Tick>(), source("/T1H/R/Out"), target("/T1H/R/P/hist"))));

struct SourceHistSM : SourceHistCtx, HSM<source_hist_model, SourceHistSM> {};

// ---------------------------------------------------------------------------
// 15. source() + choice pseudostate model (Gap 2)
// ---------------------------------------------------------------------------

struct SourceChoiceCtx {
  int value{0};
  std::string chosen;
};

constexpr auto source_choice_model = define(
    "T1C",
    initial(target("/T1C/P")),
    state("P",
          initial(target("/T1C/P/A")),
          state("A"),
          state("Pos",
                entry([](SourceChoiceCtx& c) { c.chosen = "positive"; })),
          state("Neg",
                entry([](SourceChoiceCtx& c) { c.chosen = "negative"; })),
          choice("pick",
                 transition(guard([](SourceChoiceCtx& c) { return c.value > 0; }),
                            target("/T1C/P/Pos")),
                 transition(target("/T1C/P/Neg"))),
          // source()-routed Go from A → choice
          transition(on<Go>(), source("/T1C/P/A"), target("/T1C/P/pick"))));

struct SourceChoiceSM : SourceChoiceCtx, HSM<source_choice_model, SourceChoiceSM> {};

// ---------------------------------------------------------------------------
// 16. source() + completion chaining model (Gap 3)
// ---------------------------------------------------------------------------

struct SourceCompCtx {
  std::vector<std::string> trace;
};

constexpr auto source_comp_model = define(
    "T1K",
    initial(target("/T1K/P")),
    state("P",
          initial(target("/T1K/P/A")),
          state("A",
                entry([](SourceCompCtx& c) { c.trace.push_back("enter:A"); })),
          state("B",
                entry([](SourceCompCtx& c) { c.trace.push_back("enter:B"); }),
                // Completion: auto-transition to C
                transition(target("/T1K/P/C"))),
          state("C",
                entry([](SourceCompCtx& c) { c.trace.push_back("enter:C"); })),
          // source()-routed: Go from A→B
          transition(on<Go>(), source("/T1K/P/A"), target("/T1K/P/B"))));

struct SourceCompSM : SourceCompCtx, HSM<source_comp_model, SourceCompSM> {};

// ---------------------------------------------------------------------------
// 17. source() + defer model (Gap 4) — dispatch+resume only
// ---------------------------------------------------------------------------

struct SourceDeferCtx {
  int tick_count{0};
};

constexpr auto source_defer_model = define(
    "T1D",
    initial(target("/T1D/P")),
    state("P",
          initial(target("/T1D/P/A")),
          state("A",
                defer<Tick>()),
          state("B"),
          state("C"),
          // source()-routed: Tick from B → C
          transition(on<Tick>(), source("/T1D/P/B"),
                     effect([](SourceDeferCtx& c) { ++c.tick_count; }),
                     target("/T1D/P/C")),
          // Go from A → B
          transition(on<Go>(), source("/T1D/P/A"), target("/T1D/P/B"))));

struct SourceDeferSM : SourceDeferCtx, HSM<source_defer_model, SourceDeferSM> {};

// ---------------------------------------------------------------------------
// 18. source() deep nesting model (Gap 5)
// ---------------------------------------------------------------------------

constexpr auto source_deep_nest_model = define(
    "T1N",
    initial(target("/T1N/R")),
    state("R",
          initial(target("/T1N/R/P")),
          state("P",
                initial(target("/T1N/R/P/C")),
                state("C",
                      initial(target("/T1N/R/P/C/L")),
                      state("L"))),
          state("X"),
          // source() at great-grandparent level targeting deep leaf
          transition(on<Go>(), source("/T1N/R/P/C/L"), target("/T1N/R/X"))));

struct SourceDeepNestSM : HSM<source_deep_nest_model, SourceDeepNestSM> {};

// ---------------------------------------------------------------------------
// 19. source() specificity model (Gap 6) — overlapping ancestors
// ---------------------------------------------------------------------------

struct SourceSpecCtx {
  std::string which;
};

constexpr auto source_spec_model = define(
    "T1S",
    initial(target("/T1S/P")),
    state("P",
          initial(target("/T1S/P/A")),
          state("A",
                initial(target("/T1S/P/A/X")),
                state("X"),
                state("Y")),
          state("B",
                entry([](SourceSpecCtx& c) { c.which = "B"; })),
          state("C",
                entry([](SourceSpecCtx& c) { c.which = "C"; })),
          // More specific source: leaf X
          transition(on<Go>(), source("/T1S/P/A/X"), target("/T1S/P/B")),
          // Less specific source: composite A (matches any descendant of A)
          transition(on<Go>(), source("/T1S/P/A"), target("/T1S/P/C"))));

struct SourceSpecSM : SourceSpecCtx, HSM<source_spec_model, SourceSpecSM> {};

// ---------------------------------------------------------------------------
// 20. process() + history model (Gap 7)
// ---------------------------------------------------------------------------

constexpr auto process_hist_model = define(
    "PH",
    initial(target("/PH/Out")),
    state("Out",
          transition(on<Go>(), target("/PH/C")),
          transition(on<Tick>(), target("/PH/C/hist"))),
    state("C",
          initial(target("/PH/C/A")),
          shallow_history("hist", transition(target("/PH/C/A"))),
          state("A",
                transition(on<Toggle>(), target("/PH/C/B"))),
          state("B"),
          transition(on<Stop>(), target("/PH/Out"))));

struct ProcessHistSM : HSM<process_hist_model, ProcessHistSM> {};

// ---------------------------------------------------------------------------
// 21. process() + choice model (Gap 8)
// ---------------------------------------------------------------------------

struct ProcessChoiceCtx {
  int value{0};
};

constexpr auto process_choice_model = define(
    "PC",
    initial(target("/PC/start")),
    state("start",
          transition(on<Go>(), target("/PC/pick"))),
    choice("pick",
           transition(guard([](ProcessChoiceCtx& c) { return c.value > 0; }),
                      target("/PC/pos")),
           transition(target("/PC/neg"))),
    state("pos"),
    state("neg"));

struct ProcessChoiceSM : ProcessChoiceCtx, HSM<process_choice_model, ProcessChoiceSM> {};

// ---------------------------------------------------------------------------
// 22. process() + defer model (Gap 9)
// ---------------------------------------------------------------------------

struct ProcessDeferCtx {
  int tick_handled{0};
};

constexpr auto process_defer_model = define(
    "PD",
    initial(target("/PD/A")),
    state("A",
          defer<Tick>(),
          transition(on<Go>(), target("/PD/B"))),
    state("B",
          transition(on<Tick>(),
                     effect([](ProcessDeferCtx& c) { ++c.tick_handled; }),
                     target("/PD/C"))),
    state("C"));

struct ProcessDeferSM : ProcessDeferCtx, HSM<process_defer_model, ProcessDeferSM> {};

// ---------------------------------------------------------------------------
// 23. process() + final state model (Gap 10)
// ---------------------------------------------------------------------------

struct ProcessFinalCtx {
  int entry_count{0};
};

constexpr auto process_final_model = define(
    "PF",
    initial(target("/PF/A")),
    state("A",
          entry([](ProcessFinalCtx& c) { ++c.entry_count; }),
          transition(on<Go>(), target("/PF/end"))),
    final("end"));

struct ProcessFinalSM : ProcessFinalCtx, HSM<process_final_model, ProcessFinalSM> {};

// ---------------------------------------------------------------------------
// 24. source() guard+effect skip on source miss (Gap 11)
// ---------------------------------------------------------------------------

struct SourceSkipCtx {
  int guard_calls{0};
  int effect_calls{0};
};

constexpr auto source_skip_model = define(
    "T2S",
    initial(target("/T2S/P")),
    state("P",
          initial(target("/T2S/P/A")),
          state("A"),
          state("B"),
          transition(on<Go>(), source("/T2S/P/A"),
                     guard([](SourceSkipCtx& c) {
                       ++c.guard_calls;
                       return true;
                     }),
                     effect([](SourceSkipCtx& c) { ++c.effect_calls; }),
                     target("/T2S/P/B")),
          transition(on<Stop>(), source("/T2S/P/B"),
                     target("/T2S/P/A"))));

struct SourceSkipSM : SourceSkipCtx, HSM<source_skip_model, SourceSkipSM> {};

// ---------------------------------------------------------------------------
// 25. source() + wildcard AnyEvent (Gap 12)
// ---------------------------------------------------------------------------

struct SourceWildCtx {
  int wild_count{0};
};

constexpr auto source_wild_model = define(
    "T2W",
    initial(target("/T2W/P")),
    state("P",
          initial(target("/T2W/P/A")),
          state("A"),
          state("B"),
          // AnyEvent + source(): fires for any event type when in A
          transition(on<AnyEvent>(), source("/T2W/P/A"),
                     effect([](SourceWildCtx& c) { ++c.wild_count; }),
                     target("/T2W/P/B"))));

struct SourceWildSM : SourceWildCtx, HSM<source_wild_model, SourceWildSM> {};

// ---------------------------------------------------------------------------
// 26. source() + double completion chain (Gap 13)
// ---------------------------------------------------------------------------

struct SourceChainCtx {
  std::vector<std::string> trace;
};

constexpr auto source_chain_model = define(
    "T2C",
    initial(target("/T2C/P")),
    state("P",
          initial(target("/T2C/P/A")),
          state("A",
                entry([](SourceChainCtx& c) { c.trace.push_back("enter:A"); })),
          state("B",
                entry([](SourceChainCtx& c) { c.trace.push_back("enter:B"); }),
                // Completion B→C
                transition(target("/T2C/P/C"))),
          state("C",
                entry([](SourceChainCtx& c) { c.trace.push_back("enter:C"); }),
                // Completion C→D
                transition(target("/T2C/P/D"))),
          state("D",
                entry([](SourceChainCtx& c) { c.trace.push_back("enter:D"); })),
          // source()-routed: Go from A→B
          transition(on<Go>(), source("/T2C/P/A"), target("/T2C/P/B"))));

struct SourceChainSM : SourceChainCtx, HSM<source_chain_model, SourceChainSM> {};

// ---------------------------------------------------------------------------
// 27. source() on root state (Gap 14)
// ---------------------------------------------------------------------------

struct SourceRootCtx {
  int count{0};
};

constexpr auto source_root_model = define(
    "T2R",
    initial(target("/T2R/P")),
    state("P",
          initial(target("/T2R/P/A")),
          state("A"),
          state("B"),
          // source() on the root machine state — matches any descendant
          transition(on<Go>(), source("/T2R"),
                     effect([](SourceRootCtx& c) { ++c.count; }),
                     target("/T2R/P/B"))));

struct SourceRootSM : SourceRootCtx, HSM<source_root_model, SourceRootSM> {};

// ---------------------------------------------------------------------------
// 28. source() + conflicting guards (Gap 15)
// ---------------------------------------------------------------------------

struct SourceConflictCtx {
  bool flag_a{false};
  bool flag_b{false};
  std::string which;
};

constexpr auto source_conflict_model = define(
    "T2F",
    initial(target("/T2F/P")),
    state("P",
          initial(target("/T2F/P/A")),
          state("A"),
          state("B",
                entry([](SourceConflictCtx& c) { c.which = "B"; })),
          state("C",
                entry([](SourceConflictCtx& c) { c.which = "C"; })),
          // Same event, same source, different guards
          transition(on<Go>(), source("/T2F/P/A"),
                     guard([](SourceConflictCtx& c) { return c.flag_a; }),
                     target("/T2F/P/B")),
          transition(on<Go>(), source("/T2F/P/A"),
                     guard([](SourceConflictCtx& c) { return c.flag_b; }),
                     target("/T2F/P/C"))));

struct SourceConflictSM : SourceConflictCtx, HSM<source_conflict_model, SourceConflictSM> {};

}  // namespace

// ============================================================================
// Tests
// ============================================================================

// --- Basic state transitions ---

TEST_CASE("process - transitions state on each call") {
  TwoStateSM sm;
  sm.start();
  CHECK(sm.state() == "/TS/Off");

  sm.process<Go>();
  CHECK(sm.state() == "/TS/On");

  sm.process<Stop>();
  CHECK(sm.state() == "/TS/Off");
}

TEST_CASE("process - fires entry and exit actions") {
  TwoStateSM sm;
  sm.start();
  // start enters initial state
  REQUIRE(sm.trace.size() == 1);
  CHECK(sm.trace[0] == "enter:Off");
  CHECK(sm.entry_count == 1);
  CHECK(sm.exit_count == 0);

  sm.process<Go>();
  // exit:Off, enter:On
  REQUIRE(sm.trace.size() == 3);
  CHECK(sm.trace[1] == "exit:Off");
  CHECK(sm.trace[2] == "enter:On");
  CHECK(sm.entry_count == 2);
  CHECK(sm.exit_count == 1);

  sm.process<Stop>();
  // exit:On, enter:Off
  REQUIRE(sm.trace.size() == 5);
  CHECK(sm.trace[3] == "exit:On");
  CHECK(sm.trace[4] == "enter:Off");
  CHECK(sm.entry_count == 3);
  CHECK(sm.exit_count == 2);
}

TEST_CASE("process - entry/exit counts accumulate over many cycles") {
  TwoStateSM sm;
  sm.start();

  constexpr int N = 1000;
  for (int i = 0; i < N; ++i) {
    sm.process<Go>();
    sm.process<Stop>();
  }

  CHECK(sm.state() == "/TS/Off");
  // 1 initial entry + N Go entries + N Stop entries = 1 + 2N
  CHECK(sm.entry_count == 1 + 2 * N);
  // N exits from Off + N exits from On = 2N
  CHECK(sm.exit_count == 2 * N);
  // Each cycle adds 4 trace entries (exit+enter+exit+enter)
  CHECK(sm.trace.size() == 1 + 4 * static_cast<std::size_t>(N));
}

TEST_CASE("process - unhandled event does not change state") {
  TwoStateSM sm;
  sm.start();
  CHECK(sm.state() == "/TS/Off");

  // Stop is not handled in Off state
  sm.process<Stop>();
  CHECK(sm.state() == "/TS/Off");
  // Only the initial entry, no additional actions
  CHECK(sm.trace.size() == 1);
}

// --- Guards ---

TEST_CASE("process - guard blocks transition") {
  PipeSM sm;
  sm.start();

  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");

  // Tick with empty buffer: guard rejects
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 0);
}

TEST_CASE("process - guard allows transition when condition met") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d{}; d.payload = 42;
  sm.process(d);
  CHECK(sm.buffer.size() == 1);

  sm.process<Tick>();
  // busy processes buffer and auto-returns to idle via completion
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 42);
  CHECK(sm.buffer.empty());
}

TEST_CASE("process - guard toggle produces exact 50% transition rate") {
  GuardSM sm;
  sm.start();
  CHECK(sm.state() == "/GM/S");

  constexpr int N = 10000;
  for (int i = 0; i < N; ++i) {
    sm.process<Toggle>();
  }

  // Guard toggles flag each call; transition fires when flag becomes true.
  // Odd calls (1st, 3rd, ...) set flag=true and transition.
  // Even calls (2nd, 4th, ...) set flag=false and don't transition.
  CHECK(sm.transition_count == N / 2);
}

// --- Effects with event data ---

TEST_CASE("process - effect receives event payload") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d1{}; d1.payload = 10;
  DataEvt d2{}; d2.payload = 20;
  DataEvt d3{}; d3.payload = 30;

  sm.process(d1);
  sm.process(d2);
  sm.process(d3);

  REQUIRE(sm.buffer.size() == 3);
  CHECK(sm.buffer[0] == 10);
  CHECK(sm.buffer[1] == 20);
  CHECK(sm.buffer[2] == 30);

  // Check trace
  bool found10 = false, found20 = false, found30 = false;
  for (const auto& t : sm.trace) {
    if (t == "data:10") found10 = true;
    if (t == "data:20") found20 = true;
    if (t == "data:30") found30 = true;
  }
  CHECK(found10);
  CHECK(found20);
  CHECK(found30);
}

// --- Completion transitions ---

TEST_CASE("process - completion transition fires automatically") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  DataEvt d{}; d.payload = 7;
  sm.process(d);

  // Tick transitions to busy, which processes and auto-returns to idle
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 7);

  // Verify the trace shows busy entry then idle re-entry
  bool found_processed = false;
  bool found_idle_after = false;
  for (std::size_t i = 0; i < sm.trace.size(); ++i) {
    if (sm.trace[i] == "processed:7") {
      found_processed = true;
      // The next trace entry after processed should be enter:idle
      if (i + 1 < sm.trace.size()) {
        found_idle_after = (sm.trace[i + 1] == "enter:idle");
      }
    }
  }
  CHECK(found_processed);
  CHECK(found_idle_after);
}

// --- Internal transitions ---

TEST_CASE("process - internal transition does not fire entry/exit") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  std::size_t trace_before = sm.trace.size();
  std::string last_entry = sm.trace.back();
  CHECK(last_entry == "enter:idle");

  DataEvt d{}; d.payload = 99;
  sm.process(d);

  // Internal transition adds only the effect trace, no exit/entry
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.trace.size() == trace_before + 1);
  CHECK(sm.trace.back() == "data:99");
}

// --- Hierarchical transitions ---

TEST_CASE("process - hierarchical entry order is outside-in") {
  HierSM sm;
  sm.start();

  // Initial: enter P, then enter C1
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "enter:P");
  CHECK(sm.trace[1] == "enter:C1");
  CHECK(sm.state() == "/H/P/C1");
}

TEST_CASE("process - hierarchical transition fires child exit/entry") {
  HierSM sm;
  sm.start();
  sm.trace.clear();

  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // C1→C2 within P: exit C1, enter C2 (P stays active)
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "exit:C1");
  CHECK(sm.trace[1] == "enter:C2");
  // P should NOT have been exited/re-entered
  for (const auto& t : sm.trace) {
    CHECK(t != "exit:P");
    CHECK(t != "enter:P");
  }
}

TEST_CASE("process - hierarchical round-trip preserves state") {
  HierSM sm;
  sm.start();

  for (int i = 0; i < 100; ++i) {
    sm.process<Go>();
    CHECK(sm.state() == "/H/P/C2");
    sm.process<Stop>();
    CHECK(sm.state() == "/H/P/C1");
  }
}

// --- Traffic light ring ---

TEST_CASE("process - traffic light completes full ring") {
  // First verify the ring works with dispatch+resume
  {
    TrafficSM sm;
    auto task = sm.start();
    REQUIRE(sm.state() == "/TL/Op/NS/Green");

    sm.dispatch(Tick{});
    task.resume();
    REQUIRE(sm.state() == "/TL/Op/NS/Yellow");

    sm.dispatch(Tick{});
    task.resume();
    // If this fails, the model itself has the bug (not process())
    CHECK(sm.state() == "/TL/Op/AllRed1");
  }

  // Now test via process()
  {
    TrafficSM sm;
    sm.start();
    REQUIRE(sm.state() == "/TL/Op/NS/Green");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/NS/Yellow");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/AllRed1");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/EW/Green");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/EW/Yellow");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/AllRed2");

    sm.process<Tick>();
    CHECK(sm.state() == "/TL/Op/NS/Green");
  }
}

TEST_CASE("process - traffic light multiple full rings") {
  TrafficSM sm;
  sm.start();
  sm.visited.clear();

  constexpr int RINGS = 100;
  for (int r = 0; r < RINGS; ++r) {
    for (int t = 0; t < 6; ++t) {
      sm.process<Tick>();
    }
    CHECK(sm.state() == "/TL/Op/NS/Green");
  }
  // 6 entries per ring
  CHECK(sm.visited.size() == 6 * static_cast<std::size_t>(RINGS));
}

// --- Bulk iteration correctness ---

TEST_CASE("process - three-state counter over many iterations") {
  CounterSM sm;
  sm.start();

  constexpr int N = 99999;  // multiple of 3 for clean cycles
  for (int i = 0; i < N; ++i) {
    sm.process<Tick>();
  }

  // N ticks = N transitions through the ring A→B→C→A→...
  CHECK(sm.transitions == N);

  // Each state entered N/3 times during transitions, plus initial entry for A
  CHECK(sm.a_entries == 1 + N / 3);
  CHECK(sm.b_entries == N / 3);
  CHECK(sm.c_entries == N / 3);

  // After N ticks (N divisible by 3), should be back at A
  CHECK(sm.state() == "/C/A");
}

TEST_CASE("process - three-state counter non-multiple-of-3") {
  CounterSM sm;
  sm.start();

  // 5 ticks: A→B→C→A→B→C
  for (int i = 0; i < 5; ++i) {
    sm.process<Tick>();
  }
  CHECK(sm.state() == "/C/C");
  CHECK(sm.transitions == 5);
  CHECK(sm.a_entries == 1 + 1);  // initial + one return
  CHECK(sm.b_entries == 2);
  CHECK(sm.c_entries == 2);
}

// --- Parity with dispatch + resume ---

TEST_CASE("process - produces identical results to dispatch+resume") {
  // Drive two machines with the same event sequence:
  // one via process(), one via dispatch()+resume().
  // Verify they end up in the same state with the same side effects.

  auto make_events = []() {
    std::vector<int> payloads = {10, 20, 30, 40, 50};
    return payloads;
  };

  // --- process() path ---
  PipeSM sm_proc;
  sm_proc.start();
  sm_proc.process<Go>();
  for (int v : make_events()) {
    DataEvt d{}; d.payload = v;
    sm_proc.process(d);
  }
  sm_proc.process<Tick>();
  sm_proc.process<Go>();  // unhandled in idle, should be no-op
  DataEvt extra{}; extra.payload = 99;
  sm_proc.process(extra);
  sm_proc.process<Stop>();

  // --- dispatch+resume path ---
  PipeSM sm_disp;
  auto task = sm_disp.start();
  sm_disp.dispatch(Go{});
  task.resume();
  for (int v : make_events()) {
    DataEvt d{}; d.payload = v;
    sm_disp.dispatch(d);
    task.resume();
  }
  sm_disp.dispatch(Tick{});
  task.resume();
  sm_disp.dispatch(Go{});
  task.resume();
  DataEvt extra2{}; extra2.payload = 99;
  sm_disp.dispatch(extra2);
  task.resume();
  sm_disp.dispatch(Stop{});
  task.resume();

  // --- Compare results ---
  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.processed_sum == sm_disp.processed_sum);
  CHECK(sm_proc.buffer == sm_disp.buffer);
  CHECK(sm_proc.trace == sm_disp.trace);
}

TEST_CASE("process - parity with dispatch+resume: guard toggle") {
  GuardSM sm_proc;
  sm_proc.start();

  GuardSM sm_disp;
  auto task = sm_disp.start();

  constexpr int N = 500;
  for (int i = 0; i < N; ++i) {
    sm_proc.process<Toggle>();
    sm_disp.dispatch(Toggle{});
    task.resume();
  }

  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.flag == sm_disp.flag);
  CHECK(sm_proc.transition_count == sm_disp.transition_count);
}

TEST_CASE("process - parity with dispatch+resume: traffic ring") {
  TrafficSM sm_proc;
  sm_proc.start();

  TrafficSM sm_disp;
  auto task = sm_disp.start();

  constexpr int TICKS = 42;
  for (int i = 0; i < TICKS; ++i) {
    sm_proc.process<Tick>();
    sm_disp.dispatch(Tick{});
    task.resume();
  }

  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.visited == sm_disp.visited);
}

// --- Full pipeline lifecycle via process() ---

TEST_CASE("process - full pipeline lifecycle with trace verification") {
  PipeSM sm;
  sm.start();
  CHECK(sm.state() == "/pipe/off");
  CHECK(sm.trace.back() == "enter:off");

  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.trace.back() == "enter:idle");

  // Feed data
  for (int v = 1; v <= 5; ++v) {
    DataEvt d{}; d.payload = v;
    sm.process(d);
    CHECK(sm.state() == "/pipe/idle");
  }
  CHECK(sm.buffer.size() == 5);

  // Process: transitions to busy, processes, auto-returns
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 15);  // 1+2+3+4+5
  CHECK(sm.buffer.empty());

  // Reset: self-transition on idle
  sm.process<Reset>();
  CHECK(sm.state() == "/pipe/idle");

  // Power off
  sm.process<Stop>();
  CHECK(sm.state() == "/pipe/off");
  CHECK(sm.trace.back() == "enter:off");

  // Verify trace is non-trivial (proves actions actually ran)
  CHECK(sm.trace.size() > 10);
}

// --- Stress: interleaved guard/data/process cycles ---

TEST_CASE("process - stress pipeline over many batches") {
  PipeSM sm;
  sm.start();
  sm.process<Go>();

  int expected_sum = 0;

  for (int batch = 1; batch <= 200; ++batch) {
    // Feed batch_size items
    int batch_sum = 0;
    for (int j = 0; j < batch % 7 + 1; ++j) {
      int val = batch * 10 + j;
      DataEvt d{}; d.payload = val;
      sm.process(d);
      batch_sum += val;
    }

    // Process
    sm.process<Tick>();
    expected_sum += batch_sum;
    CHECK(sm.state() == "/pipe/idle");
    CHECK(sm.buffer.empty());
  }

  CHECK(sm.processed_sum == expected_sum);
}

// --- Edge cases ---

TEST_CASE("process - calling process before start does not crash") {
  TwoStateSM sm;
  // process() before start() should be safe (no-op, state not initialized)
  sm.process<Go>();
  sm.process<Stop>();
  // Now start normally
  sm.start();
  CHECK(sm.state() == "/TS/Off");
  // Verify actions from start, not from the pre-start process calls
  CHECK(sm.entry_count == 1);
}

TEST_CASE("process - interleaving process and dispatch+resume") {
  PipeSM sm;
  auto task = sm.start();

  // Use process() for some events
  sm.process<Go>();
  CHECK(sm.state() == "/pipe/idle");

  // Use dispatch+resume for others
  DataEvt d{}; d.payload = 77;
  sm.dispatch(d);
  task.resume();
  CHECK(sm.buffer.size() == 1);
  CHECK(sm.buffer[0] == 77);

  // Back to process
  sm.process<Tick>();
  CHECK(sm.state() == "/pipe/idle");
  CHECK(sm.processed_sum == 77);
}

// --- source() DSL tests ---

TEST_CASE("process - source() routes transitions by source state") {
  // The hier_model (section 4 above) already uses source() at the parent
  // level. This test explicitly verifies the exact pattern that was broken
  // before: multiple transitions sharing the same event at a parent level
  // with different source() specs.
  HierSM sm;
  sm.start();
  CHECK(sm.state() == "/H/P/C1");

  // Go transitions from C1 -> C2 (source="/H/P/C1")
  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // Go should NOT transition from C2 (no source="/H/P/C2" for Go)
  sm.process<Go>();
  CHECK(sm.state() == "/H/P/C2");

  // Stop transitions from C2 -> C1 (source="/H/P/C2")
  sm.process<Stop>();
  CHECK(sm.state() == "/H/P/C1");

  // Stop should NOT transition from C1 (no source="/H/P/C1" for Stop)
  sm.process<Stop>();
  CHECK(sm.state() == "/H/P/C1");
}

TEST_CASE("process - source() at parent level drives traffic ring correctly") {
  // This verifies that all 6 source()-routed transitions in the traffic
  // model fire in the correct sequence.
  TrafficSM sm;
  sm.start();
  sm.visited.clear();
  CHECK(sm.state() == "/TL/Op/NS/Green");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/NS/Yellow");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/AllRed1");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/EW/Green");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/EW/Yellow");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/AllRed2");

  sm.process<Tick>();
  CHECK(sm.state() == "/TL/Op/NS/Green");

  // Verify entry actions fired for each visited state
  REQUIRE(sm.visited.size() == 6);
  CHECK(sm.visited[0] == "NS_Yellow");
  CHECK(sm.visited[1] == "AllRed1");
  CHECK(sm.visited[2] == "EW_Green");
  CHECK(sm.visited[3] == "EW_Yellow");
  CHECK(sm.visited[4] == "AllRed2");
  CHECK(sm.visited[5] == "NS_Green");
}

// --- source() + guard ---

TEST_CASE("process - source() with guard blocks transition") {
  SourceGuardSM sm;
  sm.start();
  CHECK(sm.state() == "/SG/P/A");

  // Guard blocks: allow=false, source matches (in A), but guard rejects
  sm.allow = false;
  sm.process<Go>();
  CHECK(sm.state() == "/SG/P/A");
  CHECK(sm.guard_eval_count == 1);  // guard was evaluated

  // Guard allows: allow=true
  sm.allow = true;
  sm.guard_eval_count = 0;
  sm.process<Go>();
  CHECK(sm.state() == "/SG/P/B");
  CHECK(sm.guard_eval_count == 1);  // guard was evaluated and passed
}

TEST_CASE("process - source() with guard allows transition") {
  SourceGuardSM sm;
  sm.start();
  sm.allow = true;
  CHECK(sm.state() == "/SG/P/A");

  sm.process<Go>();
  CHECK(sm.state() == "/SG/P/B");
  CHECK(sm.guard_eval_count == 1);

  // Go while in B: source doesn't match, guard should NOT be evaluated
  sm.guard_eval_count = 0;
  sm.process<Go>();
  CHECK(sm.state() == "/SG/P/B");  // no-op
  CHECK(sm.guard_eval_count == 0);  // guard not evaluated — source miss
}

// --- source() + effect ---

TEST_CASE("process - source() with effect fires correct effect per source") {
  SourceEffectSM sm;
  sm.start();
  CHECK(sm.state() == "/SE/P/A");

  // From A: effect "A->B" fires
  sm.process<Go>();
  CHECK(sm.state() == "/SE/P/B");
  CHECK(sm.last_effect == "A->B");
  CHECK(sm.effect_count == 1);

  // From B: effect "B->A" fires
  sm.process<Go>();
  CHECK(sm.state() == "/SE/P/A");
  CHECK(sm.last_effect == "B->A");
  CHECK(sm.effect_count == 2);

  // Multiple round-trips
  for (int i = 0; i < 10; ++i) {
    sm.process<Go>();  // A->B
    CHECK(sm.last_effect == "A->B");
    sm.process<Go>();  // B->A
    CHECK(sm.last_effect == "B->A");
  }
  CHECK(sm.effect_count == 22);  // 2 initial + 20 in loop
}

// --- source() self-transition ---

TEST_CASE("process - source() self-transition re-enters state") {
  SourceSelfSM sm;
  sm.start();
  CHECK(sm.state() == "/SS/P/A");
  CHECK(sm.entry_count == 1);  // initial entry

  // Self-transition: Tick from A -> A
  sm.process<Tick>();
  CHECK(sm.state() == "/SS/P/A");
  CHECK(sm.entry_count == 2);  // re-entered A

  sm.process<Tick>();
  CHECK(sm.state() == "/SS/P/A");
  CHECK(sm.entry_count == 3);

  // Verify Go still works to move to B
  sm.process<Go>();
  CHECK(sm.state() == "/SS/P/B");

  // Tick while in B should be no-op (source miss)
  int prev = sm.entry_count;
  sm.process<Tick>();
  CHECK(sm.state() == "/SS/P/B");
  CHECK(sm.entry_count == prev);
}

// --- source() internal transition (no target) ---

TEST_CASE("process - source() internal transition fires effect without entry/exit") {
  SourceInternalSM sm;
  sm.start();
  CHECK(sm.state() == "/SI/P/A");
  CHECK(sm.entry_count == 1);  // initial entry of A

  // Tick with source("/SI/P/A") + effect, no target: internal transition
  sm.process<Tick>();
  CHECK(sm.action_count == 1);
  CHECK(sm.entry_count == 1);  // no re-entry
  CHECK(sm.state() == "/SI/P/A");

  // Multiple internal transitions
  sm.process<Tick>();
  sm.process<Tick>();
  CHECK(sm.action_count == 3);
  CHECK(sm.entry_count == 1);  // still no re-entry

  // Move to B
  sm.process<Go>();
  CHECK(sm.state() == "/SI/P/B");
  CHECK(sm.entry_count == 2);  // B entry counted

  // Tick while in B: source miss, no effect
  int prev_action = sm.action_count;
  sm.process<Tick>();
  CHECK(sm.action_count == prev_action);  // unchanged
  CHECK(sm.state() == "/SI/P/B");
}

// --- source() parity with dispatch+resume ---

TEST_CASE("process - source() parity with dispatch+resume") {
  // Drive HierSM with process() and dispatch()+resume() identically
  HierSM sm_proc;
  sm_proc.start();

  HierSM sm_disp;
  auto task = sm_disp.start();

  // Sequence: Go, Stop, Go, Go(noop), Stop, Stop(noop), Go
  sm_proc.process<Go>();
  sm_disp.dispatch(Go{});
  task.resume();

  sm_proc.process<Stop>();
  sm_disp.dispatch(Stop{});
  task.resume();

  sm_proc.process<Go>();
  sm_disp.dispatch(Go{});
  task.resume();

  // Go while in C2: source miss (no-op)
  sm_proc.process<Go>();
  sm_disp.dispatch(Go{});
  task.resume();

  sm_proc.process<Stop>();
  sm_disp.dispatch(Stop{});
  task.resume();

  // Stop while in C1: source miss (no-op)
  sm_proc.process<Stop>();
  sm_disp.dispatch(Stop{});
  task.resume();

  sm_proc.process<Go>();
  sm_disp.dispatch(Go{});
  task.resume();

  CHECK(sm_proc.state() == sm_disp.state());
  CHECK(sm_proc.trace == sm_disp.trace);
}

// --- Mixed structural + source() transitions ---

TEST_CASE("process - mixed structural and source() transitions coexist") {
  SourceMixedSM sm;
  sm.start();
  CHECK(sm.state() == "/SM/P/A");

  // Structural transition: Tick inside A -> B
  sm.process<Tick>();
  CHECK(sm.state() == "/SM/P/B");

  // source()-based: Go from B -> C
  sm.process<Go>();
  CHECK(sm.state() == "/SM/P/C");

  // source()-based: Stop from C -> A
  sm.process<Stop>();
  CHECK(sm.state() == "/SM/P/A");

  // Full round-trip A -> B -> C -> A
  sm.trace.clear();
  sm.process<Tick>();
  CHECK(sm.state() == "/SM/P/B");
  sm.process<Go>();
  CHECK(sm.state() == "/SM/P/C");
  sm.process<Stop>();
  CHECK(sm.state() == "/SM/P/A");

  // Verify all entries fired
  REQUIRE(sm.trace.size() == 3);
  CHECK(sm.trace[0] == "enter:B");
  CHECK(sm.trace[1] == "enter:C");
  CHECK(sm.trace[2] == "enter:A");
}

// --- source() entry/exit ordering ---

TEST_CASE("process - source() preserves correct entry/exit order") {
  SourceEntryExitSM sm;
  sm.start();
  // Initial: enter:P, enter:A
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "enter:P");
  CHECK(sm.trace[1] == "enter:A");
  CHECK(sm.state() == "/EO/P/A");

  // Go: A -> B within P. P should NOT be exited/re-entered.
  sm.trace.clear();
  sm.process<Go>();
  CHECK(sm.state() == "/EO/P/B");
  REQUIRE(sm.trace.size() == 2);
  CHECK(sm.trace[0] == "exit:A");
  CHECK(sm.trace[1] == "enter:B");
  // Confirm P was not touched
  for (const auto& t : sm.trace) {
    CHECK(t != "exit:P");
    CHECK(t != "enter:P");
  }

  // Stop: B -> A within P. Same ordering expectation.
  sm.trace.clear();
  sm.process<Stop>();
  CHECK(sm.state() == "/EO/P/A");
  REQUIRE(sm.trace.size() == 2);
  CHECK(sm.trace[0] == "exit:B");
  CHECK(sm.trace[1] == "enter:A");
  for (const auto& t : sm.trace) {
    CHECK(t != "exit:P");
    CHECK(t != "enter:P");
  }
}

// ============================================================================
// Tier 1 gap tests — source() combinations and process() cross-feature
// ============================================================================

// --- Gap 1: source() + history target ---

TEST_CASE("source() + history target restores previous state") {
  SourceHistSM sm;
  sm.start();
  CHECK(sm.state() == "/T1H/R/P/A");

  // Navigate A→B
  sm.process<Go>();
  CHECK(sm.state() == "/T1H/R/P/B");

  // Exit P → Out (history should remember B)
  sm.process<Stop>();
  CHECK(sm.state() == "/T1H/R/Out");

  // Re-enter via source()-routed transition targeting history
  sm.trace.clear();
  sm.process<Tick>();
  CHECK(sm.state() == "/T1H/R/P/B");  // history restored, not initial A

  // Verify B's entry fired
  bool found_b = false;
  for (const auto& t : sm.trace) {
    if (t == "enter:B") found_b = true;
  }
  CHECK(found_b);
}

TEST_CASE("source() + history target uses default on first entry") {
  SourceHistSM sm;
  sm.start();
  CHECK(sm.state() == "/T1H/R/P/A");

  // Exit without ever visiting B
  sm.process<Stop>();
  CHECK(sm.state() == "/T1H/R/Out");

  // Re-enter via history — no prior recorded, should use default (A)
  sm.process<Tick>();
  CHECK(sm.state() == "/T1H/R/P/A");
}

// --- Gap 2: source() + choice pseudostate ---

TEST_CASE("source() + choice pseudostate routes correctly") {
  // Positive path
  {
    SourceChoiceSM sm;
    sm.start();
    CHECK(sm.state() == "/T1C/P/A");

    sm.value = 5;
    sm.process<Go>();
    CHECK(sm.state() == "/T1C/P/Pos");
    CHECK(sm.chosen == "positive");
  }

  // Negative path (fallback)
  {
    SourceChoiceSM sm;
    sm.start();
    CHECK(sm.state() == "/T1C/P/A");

    sm.value = -3;
    sm.process<Go>();
    CHECK(sm.state() == "/T1C/P/Neg");
    CHECK(sm.chosen == "negative");
  }

  // Source miss: Go while not in A → no-op
  {
    SourceChoiceSM sm;
    sm.start();
    sm.value = 5;
    sm.process<Go>();  // A→Pos
    CHECK(sm.state() == "/T1C/P/Pos");

    sm.process<Go>();  // from Pos: source miss, no-op
    CHECK(sm.state() == "/T1C/P/Pos");
  }
}

// --- Gap 3: source() + completion chaining ---

TEST_CASE("source() + completion transition chains in single process") {
  SourceCompSM sm;
  sm.start();
  sm.trace.clear();
  CHECK(sm.state() == "/T1K/P/A");

  // Go from A→B via source(), then B→C via completion
  sm.process<Go>();
  CHECK(sm.state() == "/T1K/P/C");

  // Verify trace: B was entered (completion fired), then C was entered
  REQUIRE(sm.trace.size() >= 2);
  CHECK(sm.trace[0] == "enter:B");
  CHECK(sm.trace[1] == "enter:C");
}

// --- Gap 4: source() + defer ---

TEST_CASE("source() + defer replays with correct source match") {
  // Must use dispatch()+resume() since deferral requires the event queue
  SourceDeferSM sm;
  auto task = sm.start();
  CHECK(sm.state() == "/T1D/P/A");

  // Tick in A → deferred (A declares defer<Tick>())
  sm.dispatch(Tick{});
  task.resume();
  CHECK(sm.state() == "/T1D/P/A");  // still in A, Tick deferred

  // Go: A→B via source(). Deferred Tick replays.
  // Tick replays in B: source("/T1D/P/B") matches → B→C
  sm.dispatch(Go{});
  task.resume();
  CHECK(sm.state() == "/T1D/P/C");
  CHECK(sm.tick_count == 1);  // effect on Tick fired
}

// --- Gap 5: source() with deep nesting ---

TEST_CASE("source() with deep nesting matches grandchild from great-grandparent") {
  SourceDeepNestSM sm;
  sm.start();
  // Initial drills down: R → P → C → L
  CHECK(sm.state() == "/T1N/R/P/C/L");

  // Go: source("/T1N/R/P/C/L") at R level → X
  sm.process<Go>();
  CHECK(sm.state() == "/T1N/R/X");
}

// --- Gap 6: source() specificity ---

TEST_CASE("source() specificity: leaf source wins over ancestor source") {
  // From X: specific leaf source wins → B
  SourceSpecSM sm;
  sm.start();
  CHECK(sm.state() == "/T1S/P/A/X");  // initial drills to X

  sm.process<Go>();
  CHECK(sm.state() == "/T1S/P/B");
  CHECK(sm.which == "B");
}

// --- Gap 7: process() + history ---

TEST_CASE("process() + history restores previous state") {
  ProcessHistSM sm;
  sm.start();
  CHECK(sm.state() == "/PH/Out");

  // Enter container → C/A
  sm.process<Go>();
  CHECK(sm.state() == "/PH/C/A");

  // Navigate to B
  sm.process<Toggle>();
  CHECK(sm.state() == "/PH/C/B");

  // Exit → Out (history remembers B)
  sm.process<Stop>();
  CHECK(sm.state() == "/PH/Out");

  // Re-enter via history → B
  sm.process<Tick>();
  CHECK(sm.state() == "/PH/C/B");

  // Round-trip: exit again, re-enter via history again
  sm.process<Stop>();
  CHECK(sm.state() == "/PH/Out");
  sm.process<Tick>();
  CHECK(sm.state() == "/PH/C/B");
}

TEST_CASE("process() + history uses default on first entry") {
  ProcessHistSM sm;
  sm.start();
  CHECK(sm.state() == "/PH/Out");

  // Re-enter via history without prior visit → default (A)
  sm.process<Tick>();
  CHECK(sm.state() == "/PH/C/A");
}

// --- Gap 8: process() + choice ---

TEST_CASE("process() + choice pseudostate routes correctly") {
  // Positive path
  {
    ProcessChoiceSM sm;
    sm.start();
    CHECK(sm.state() == "/PC/start");

    sm.value = 10;
    sm.process<Go>();
    CHECK(sm.state() == "/PC/pos");
  }

  // Negative path (fallback)
  {
    ProcessChoiceSM sm;
    sm.start();
    CHECK(sm.state() == "/PC/start");

    sm.value = -5;
    sm.process<Go>();
    CHECK(sm.state() == "/PC/neg");
  }

  // Zero (boundary — not positive, falls to negative)
  {
    ProcessChoiceSM sm;
    sm.start();
    sm.value = 0;
    sm.process<Go>();
    CHECK(sm.state() == "/PC/neg");
  }
}

// --- Gap 9: process() bypasses deferral ---

TEST_CASE("process() bypasses deferral - event not queued for later") {
  // With process(): Tick is processed inline, bypassing defer.
  // In state A, Tick has no matching transition (only defer<Tick>() which
  // process() ignores), so it's a no-op. Event is lost.
  ProcessDeferSM sm_proc;
  sm_proc.start();
  CHECK(sm_proc.state() == "/PD/A");

  sm_proc.process<Tick>();  // bypasses defer - no-op, event lost
  sm_proc.process<Go>();    // A->B
  CHECK(sm_proc.state() == "/PD/B");
  CHECK(sm_proc.tick_handled == 0);  // Tick was never replayed

  // With dispatch()+resume(): Tick IS deferred and replayed after Go
  ProcessDeferSM sm_disp;
  auto task = sm_disp.start();

  sm_disp.dispatch(Tick{});  // deferred in A
  task.resume();
  CHECK(sm_disp.state() == "/PD/A");

  sm_disp.dispatch(Go{});  // A->B, deferred Tick replays -> B->C
  task.resume();
  CHECK(sm_disp.state() == "/PD/C");
  CHECK(sm_disp.tick_handled == 1);  // Tick was replayed and handled
}

// --- Gap 10: process() + final state ---

TEST_CASE("process() + final state terminates correctly") {
  ProcessFinalSM sm;
  sm.start();
  CHECK(sm.state() == "/PF/A");
  CHECK(sm.entry_count == 1);

  // Transition to final state
  sm.process<Go>();
  CHECK(sm.state() == "/PF/end");

  // Further Go events are ignored in final state
  sm.process<Go>();
  CHECK(sm.state() == "/PF/end");

  sm.process<Go>();
  CHECK(sm.state() == "/PF/end");
}

// ============================================================================
// Tier 2 gap tests — robustness and edge cases
// ============================================================================

// --- Gap 11: guard + effect skipped on source miss ---

TEST_CASE("source miss skips both guard and effect evaluation") {
  SourceSkipSM sm;
  sm.start();
  CHECK(sm.state() == "/T2S/P/A");

  // Source matches A: guard and effect should fire
  sm.process<Go>();
  CHECK(sm.state() == "/T2S/P/B");
  CHECK(sm.guard_calls == 1);
  CHECK(sm.effect_calls == 1);

  // Now in B: Go has source("/T2S/P/A") — source miss
  sm.guard_calls = 0;
  sm.effect_calls = 0;
  sm.process<Go>();
  CHECK(sm.state() == "/T2S/P/B");  // no-op
  CHECK(sm.guard_calls == 0);   // guard never called
  CHECK(sm.effect_calls == 0);  // effect never called

  // Return to A and verify guard+effect fire again
  sm.process<Stop>();
  CHECK(sm.state() == "/T2S/P/A");
  sm.guard_calls = 0;
  sm.effect_calls = 0;
  sm.process<Go>();
  CHECK(sm.state() == "/T2S/P/B");
  CHECK(sm.guard_calls == 1);
  CHECK(sm.effect_calls == 1);
}

// --- Gap 12: source() + wildcard AnyEvent ---

TEST_CASE("source() + AnyEvent wildcard matches any event type from source") {
  SourceWildSM sm;
  sm.start();
  CHECK(sm.state() == "/T2W/P/A");

  // Any event while in A triggers the wildcard+source transition
  sm.process<Go>();
  CHECK(sm.state() == "/T2W/P/B");
  CHECK(sm.wild_count == 1);
}

TEST_CASE("source() + AnyEvent wildcard does not fire on source miss") {
  SourceWildSM sm;
  sm.start();
  sm.process<Go>();  // A→B
  CHECK(sm.state() == "/T2W/P/B");

  // Now in B: source miss for AnyEvent transition (source is A)
  sm.wild_count = 0;
  sm.process<Go>();  // source miss
  CHECK(sm.state() == "/T2W/P/B");
  CHECK(sm.wild_count == 0);
}

// --- Gap 13: source() + double completion chain ---

TEST_CASE("source() triggers double completion chain A->B->C->D") {
  SourceChainSM sm;
  sm.start();
  sm.trace.clear();
  CHECK(sm.state() == "/T2C/P/A");

  // Single process<Go>(): A→B(source) → C(completion) → D(completion)
  sm.process<Go>();
  CHECK(sm.state() == "/T2C/P/D");

  // Verify all intermediate entries fired in order
  REQUIRE(sm.trace.size() == 3);
  CHECK(sm.trace[0] == "enter:B");
  CHECK(sm.trace[1] == "enter:C");
  CHECK(sm.trace[2] == "enter:D");
}

// --- Gap 14: source() on root state ---

TEST_CASE("source() on root state matches any descendant") {
  SourceRootSM sm;
  sm.start();
  CHECK(sm.state() == "/T2R/P/A");

  // source("/T2R") is the root — the walk from leaf A passes through P then T2R
  sm.process<Go>();
  CHECK(sm.state() == "/T2R/P/B");
  CHECK(sm.count == 1);
}

// --- Gap 15: source() + conflicting guards ---

TEST_CASE("source() with conflicting guards picks the passing one") {
  // Only flag_a true → first transition wins → B
  {
    SourceConflictSM sm;
    sm.start();
    CHECK(sm.state() == "/T2F/P/A");
    sm.flag_a = true;
    sm.flag_b = false;

    sm.process<Go>();
    CHECK(sm.state() == "/T2F/P/B");
    CHECK(sm.which == "B");
  }

  // Only flag_b true → second transition wins → C
  {
    SourceConflictSM sm;
    sm.start();
    CHECK(sm.state() == "/T2F/P/A");
    sm.flag_a = false;
    sm.flag_b = true;

    sm.process<Go>();
    CHECK(sm.state() == "/T2F/P/C");
    CHECK(sm.which == "C");
  }

  // Both flags true → first transition wins (evaluated first) → B
  {
    SourceConflictSM sm;
    sm.start();
    CHECK(sm.state() == "/T2F/P/A");
    sm.flag_a = true;
    sm.flag_b = true;

    sm.process<Go>();
    CHECK(sm.state() == "/T2F/P/B");
    CHECK(sm.which == "B");
  }

  // Neither flag true → both guards fail → no transition
  {
    SourceConflictSM sm;
    sm.start();
    CHECK(sm.state() == "/T2F/P/A");
    sm.flag_a = false;
    sm.flag_b = false;

    sm.process<Go>();
    CHECK(sm.state() == "/T2F/P/A");  // no-op
  }
}
