#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <random>
#include <string>

#include "hsm/hsm.hpp"

using namespace hsm;

// ============================================================================
// Gap #23: Property-based fuzz testing with random event sequences.
//
// Strategy: Generate random event sequences and assert structural invariants
// hold regardless of input. Uses std::mt19937 with fixed seeds for
// reproducibility. No external fuzzing framework needed.
//
// Model: Three-state ring with source()-routed transitions. Context tracks
// entry/exit counts per state for invariant verification.
//
// Invariants checked after every random event:
// 1. Valid state — sm.state() is one of the known state paths
// 2. Entry/exit pairing — entries >= exits, entries - exits <= 1
// 3. Exactly one active state — sum of (entries - exits) across leaves == 1
// ============================================================================

namespace {

// ---------------------------------------------------------------------------
// Events (IDs 850-859)
// ---------------------------------------------------------------------------

struct FuzzA : Event<make_kind(850, Kind::Event)> {};
struct FuzzB : Event<make_kind(851, Kind::Event)> {};
struct FuzzC : Event<make_kind(852, Kind::Event)> {};

// ---------------------------------------------------------------------------
// Model: three-state ring with source()-routed transitions
// ---------------------------------------------------------------------------
//
// Fuzz (root)
// ├── S1 (initial) — entry/exit tracked
// ├── S2 — entry/exit tracked
// └── S3 — entry/exit tracked
//
// Transitions (at root level):
//   FuzzA + source(S1) → S2
//   FuzzB + source(S2) → S3
//   FuzzC + source(S3) → S1
//   FuzzA + source(S2) → S2 (self-transition, tests re-entry)
//   FuzzB + source(S1) — internal (no target, effect-only)

struct FuzzCtx {
  int s1_entries{0};
  int s1_exits{0};
  int s2_entries{0};
  int s2_exits{0};
  int s3_entries{0};
  int s3_exits{0};
  int internal_effects{0};
};

constexpr auto fuzz_model = define(
    "F",
    initial(target("/F/S1")),
    state("S1",
          entry([](FuzzCtx& c) { ++c.s1_entries; }),
          exit([](FuzzCtx& c) { ++c.s1_exits; })),
    state("S2",
          entry([](FuzzCtx& c) { ++c.s2_entries; }),
          exit([](FuzzCtx& c) { ++c.s2_exits; })),
    state("S3",
          entry([](FuzzCtx& c) { ++c.s3_entries; }),
          exit([](FuzzCtx& c) { ++c.s3_exits; })),
    // Ring: S1 → S2 → S3 → S1
    transition(on<FuzzA>(), source("/F/S1"), target("/F/S2")),
    transition(on<FuzzB>(), source("/F/S2"), target("/F/S3")),
    transition(on<FuzzC>(), source("/F/S3"), target("/F/S1")),
    // Self-transition: S2 → S2 on FuzzA
    transition(on<FuzzA>(), source("/F/S2"), target("/F/S2")),
    // Internal transition: effect-only from S1 on FuzzB
    transition(on<FuzzB>(), source("/F/S1"),
               effect([](FuzzCtx& c) { ++c.internal_effects; })));

struct FuzzSM : FuzzCtx, HSM<fuzz_model, FuzzSM> {};

// ---------------------------------------------------------------------------
// Invariant checker
// ---------------------------------------------------------------------------

void check_invariants(const FuzzSM& sm, int iteration) {
  INFO("Iteration: " << iteration);

  // Invariant 1: Valid state
  const auto s = sm.state();
  bool valid_state = (s == "/F/S1" || s == "/F/S2" || s == "/F/S3");
  CHECK_MESSAGE(valid_state, "Invalid state: " << s);

  // Invariant 2: Entry/exit pairing per state
  CHECK(sm.s1_entries >= sm.s1_exits);
  CHECK(sm.s2_entries >= sm.s2_exits);
  CHECK(sm.s3_entries >= sm.s3_exits);
  CHECK(sm.s1_entries - sm.s1_exits <= 1);
  CHECK(sm.s2_entries - sm.s2_exits <= 1);
  CHECK(sm.s3_entries - sm.s3_exits <= 1);

  // Invariant 3: Exactly one active leaf state
  int active = (sm.s1_entries - sm.s1_exits) + (sm.s2_entries - sm.s2_exits) +
               (sm.s3_entries - sm.s3_exits);
  CHECK(active == 1);
}

// ---------------------------------------------------------------------------
// Random event dispatch helper
// ---------------------------------------------------------------------------

void dispatch_random(FuzzSM& sm, std::mt19937& rng) {
  std::uniform_int_distribution<int> dist(0, 2);
  switch (dist(rng)) {
    case 0:
      sm.process<FuzzA>();
      break;
    case 1:
      sm.process<FuzzB>();
      break;
    case 2:
      sm.process<FuzzC>();
      break;
  }
}

}  // namespace

// ============================================================================
// Tests
// ============================================================================

TEST_CASE("fuzz - fixed seed invariants hold over 10000 random events") {
  FuzzSM sm;
  sm.start();
  check_invariants(sm, 0);

  std::mt19937 rng(42);

  for (int i = 1; i <= 10000; ++i) {
    dispatch_random(sm, rng);
    check_invariants(sm, i);
  }
}

TEST_CASE("fuzz - multiple seeds") {
  for (unsigned seed = 1; seed <= 20; ++seed) {
    CAPTURE(seed);
    FuzzSM sm;
    sm.start();

    std::mt19937 rng(seed);

    for (int i = 1; i <= 1000; ++i) {
      dispatch_random(sm, rng);
      check_invariants(sm, i);
    }
  }
}

TEST_CASE("fuzz - process remains stable across random events") {
  FuzzSM sm;
  sm.start();

  std::mt19937 rng(42);
  std::uniform_int_distribution<int> dist(0, 2);

  for (int i = 0; i < 5000; ++i) {
    switch (dist(rng)) {
      case 0:
        sm.process<FuzzA>();
        break;
      case 1:
        sm.process<FuzzB>();
        break;
      default:
        sm.process<FuzzC>();
        break;
    }
  }
}
