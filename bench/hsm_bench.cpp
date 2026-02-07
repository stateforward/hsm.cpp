#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fstream>
#include <filesystem>

#include "hsm/hsm.hpp"
#include "hsm/kind.hpp"

#if defined(HSM_BENCH_ENABLE_HSMCPP)
#include <hsmcpp/hsm.hpp>
#include <hsmcpp/HsmEventDispatcherSTD.hpp>
#endif

#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/msm/front/functor_row.hpp>
#include <boost/msm/back/state_machine.hpp>
#include <boost/mpl/vector.hpp>
#include <exception>

// Some Boost.MSM configurations expect a non-header-only definition of
// boost::throw_exception(std::exception const&). When building the
// standalone benchmark we provide a minimal implementation here to avoid
// depending on a separate Boost.Exception library.
namespace boost {
inline void throw_exception(const std::exception &e) {
  (void)e;
  std::terminate();
}
} // namespace boost
#endif

#if defined(HSM_BENCH_ENABLE_HFSM2)
#include <hfsm2/machine.hpp>
#endif

#if defined(HSM_BENCH_ENABLE_QP)
#include "qpcpp.hpp"
#endif

#if defined(HSM_BENCH_ENABLE_SML)
#include <boost/sml.hpp>
#endif

// hsm_comparison_bench.cpp
// -------------------------
// Unified benchmark harness to compare hsm against other C++ HSM/FSM
// libraries and vanilla C++ baselines.
//
// This benchmark focuses on dispatch/transition throughput for a small set of
// semantically equivalent scenarios (Ping-Pong, Hierarchical, Deep, Guarded).
//
// The harness is backend-agnostic: each library provides a small adapter that
// implements the same logical scenarios. The hot path contains no logging,
// allocations, or sleeps; all timing is done with std::chrono::steady_clock.
//
// External libraries (hsmcpp, Boost.MSM, QP/C++, Boost-ext.SML, HFSM2) are
// integrated conditionally via compile-time flags:
//   - HSM_BENCH_ENABLE_HSMCPP
//   - HSM_BENCH_ENABLE_BOOST_MSM
//   - HSM_BENCH_ENABLE_QP
//   - HSM_BENCH_ENABLE_SML
//   - HSM_BENCH_ENABLE_HFSM2
//
// These flags are expected to be provided by the build system (see
// examples/CMakeLists.txt). When a flag is not defined, the corresponding
// backend is simply reported as unavailable and ignored, so this file builds
// without any external dependencies by default.
//
// NOTE: This harness currently implements the hsm backend and two vanilla
// C++ baselines (switch-based and function-pointer-based FSMs). The structure
// is designed so that additional library adapters can be added incrementally
// behind the above feature flags without changing the core harness.

using Clock = std::chrono::steady_clock;

template <typename T>
inline void doNotOptimize(T const& value) {
  asm volatile("" : : "m"(value) : "memory");
}

// -----------------------------------------------------------------------------
// Global configuration and CLI
// -----------------------------------------------------------------------------

struct GlobalConfig {
  std::size_t warmup_iterations = 1000;
  std::size_t iterations = 1000000;

  enum class Format { Text, Csv, Json, All } format = Format::All;

  // Substring filter on scenario human-readable name (legacy behavior).
  std::string scenario_filter;
  // Explicit scenario id list (comma-separated). When non-empty, this takes
  // precedence over scenario_filter and is matched against ScenarioSpec::id.
  std::string scenario_ids;
  // Comma-separated backend names.
  std::string libs_filter;
  // Library name used as baseline for relative percentage computations.
  std::string baseline = "hsm";
  // Prefix directory for CSV/JSON filenames. Defaults to ".snapshots" so that
  // benchmark runs do not clutter the project root.
  std::string output_prefix = ".snapshots";
};

static void print_usage(const char *argv0) {
  std::cout << "Usage: " << argv0
            << " [--warmup=N] [--iterations=N] [--filter=substr]"
               " [--scenarios=id1,id2,...]"
               " [--libs=list] [--baseline=name]"
               " [--format=text|csv|json|all] [--output-prefix=PATH]";
  std::cout << "\n\n";
  std::cout << "Scenarios (ids for --scenarios):\n"
            << "  ping_pong,hierarchical,deep,guarded,traffic_light\n\n";
  std::cout << "Libraries (names for --libs / --baseline):\n"
            << "  hsm,hsm_threaded,hsmcpp,boost_msm,qp,sml,hfsm2,vanilla_switch,vanilla_fp\n";
  std::cout << "\nDefault output prefix is '.snapshots' so benchmark snapshots are kept out of the project root.\n";
}

static std::optional<GlobalConfig> parse_args(int argc, char **argv) {
  GlobalConfig cfg;

  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      print_usage(argv[0]);
      return std::nullopt;
    }

    auto parse_eq = [&](const char *prefix) -> const char * {
      const std::size_t len = std::strlen(prefix);
      if (std::strncmp(arg, prefix, len) == 0 && arg[len] == '=') {
        return arg + len + 1;
      }
      return nullptr;
    };

    if (const char *val = parse_eq("--warmup")) {
      cfg.warmup_iterations = static_cast<std::size_t>(std::strtoull(val, nullptr, 10));
      continue;
    }
    if (const char *val = parse_eq("--iterations")) {
      cfg.iterations = static_cast<std::size_t>(std::strtoull(val, nullptr, 10));
      continue;
    }
    if (const char *val = parse_eq("--filter")) {
      cfg.scenario_filter = val;
      continue;
    }
    if (const char *val = parse_eq("--scenarios")) {
      cfg.scenario_ids = val;
      continue;
    }
    if (const char *val = parse_eq("--libs")) {
      cfg.libs_filter = val;  // parsed later into a set
      continue;
    }
    if (const char *val = parse_eq("--baseline")) {
      cfg.baseline = val;
      continue;
    }
    if (const char *val = parse_eq("--format")) {
      if (std::strcmp(val, "text") == 0) {
        cfg.format = GlobalConfig::Format::Text;
      } else if (std::strcmp(val, "csv") == 0) {
        cfg.format = GlobalConfig::Format::Csv;
      } else if (std::strcmp(val, "json") == 0) {
        cfg.format = GlobalConfig::Format::Json;
      } else if (std::strcmp(val, "all") == 0) {
        cfg.format = GlobalConfig::Format::All;
      } else {
        std::cerr << "Unknown --format value: " << val << "\n";
        return std::nullopt;
      }
      continue;
    }
    if (const char *val = parse_eq("--output-prefix")) {
      cfg.output_prefix = val;
      continue;
    }

    std::cerr << "Unknown argument: " << arg << "\n";
    print_usage(argv[0]);
    return std::nullopt;
  }

  return cfg;
}

// -----------------------------------------------------------------------------
// Measurement helpers
// -----------------------------------------------------------------------------

template <typename Loop>
static double measure_throughput(std::size_t warmup_iterations,
                                 std::size_t measured_iterations,
                                 std::size_t operations_per_iteration,
                                 Loop &&loop) {
  using namespace std::chrono;

  // Prevent optimization of the loop body
  auto volatile_sink = [](auto&&) {};
  (void)volatile_sink; // suppress unused variable warning

  for (std::size_t i = 0; i < warmup_iterations; ++i) {
    loop();
  }

  const auto start = Clock::now();
  for (std::size_t i = 0; i < measured_iterations; ++i) {
    loop();
    // Simple memory barrier to prevent loop unrolling/deletion optimization
    // strictly speaking standard C++ doesn't guarantee this, but it helps.
    // A better approach is returning a value from loop() and using it.
    asm volatile("" : : : "memory");
  }
  const auto end = Clock::now();

  const auto micros = duration_cast<microseconds>(end - start).count();
  if (micros == 0) {
    return 0.0;
  }

  const double total_ops = static_cast<double>(operations_per_iteration) *
                           static_cast<double>(measured_iterations);
  const double seconds = static_cast<double>(micros) / 1'000'000.0;
  return total_ops / seconds;
}

// -----------------------------------------------------------------------------
// Scenario metadata
// -----------------------------------------------------------------------------

struct ScenarioSpec {
  const char *id;                     // stable identifier, e.g. "ping_pong"
  const char *name;                   // human-readable name
  std::size_t dispatches_per_iteration;  // how many dispatch() calls per loop
  double transitions_per_dispatch;    // average transitions per dispatch
};

static const ScenarioSpec kScenarios[] = {
    {"ping_pong",
     "1. Simple Ping-Pong (A<->B, no guards)",
     2,  // E1 then E2 per iteration
     1.0},
    {"hierarchical",
     "2. Hierarchical Parent/Child (P/C1/C2 with entry/exit)",
     2,  // toggle between C1 and C2
     1.0},
    {"deep",
     "3. Deep Hierarchy (L1/L2/L3a<->L3b)",
     2,
     1.0},
    {"guarded",
     "4. Guarded Transition (every other dispatch transitions)",
     1,
     0.5},  // on average, half the dispatches perform a transition
    {"traffic_light",
     "5. Traffic Light Controller (complex hierarchy)",
     4,  // four ticks per iteration
     1.0},
};

// -----------------------------------------------------------------------------
// Backend/libraries
// -----------------------------------------------------------------------------

enum class LibraryKind {
  Hsm,
  HsmThreaded,
  Hsmcpp,
  BoostMsm,
  Qp,
  Sml,
  Hfsm2,
  VanillaSwitch,
  VanillaFp,
};

struct LibraryInfo {
  LibraryKind kind;
  const char *name;  // stable CLI name, e.g. "hsm"
  bool available;    // compile-time availability
};

static constexpr bool kHasHsmcpp =
#if defined(HSM_BENCH_ENABLE_HSMCPP)
    true;
#else
    false;
#endif

static constexpr bool kHasBoostMsm =
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    true;
#else
    false;
#endif

static constexpr bool kHasQp =
#if defined(HSM_BENCH_ENABLE_QP)
    true;
#else
    false;
#endif

static constexpr bool kHasSml =
#if defined(HSM_BENCH_ENABLE_SML)
    true;
#else
    false;
#endif

static constexpr bool kHasHfsm2 =
#if defined(HSM_BENCH_ENABLE_HFSM2)
    true;
#else
    false;
#endif

static const LibraryInfo kLibraries[] = {
    {LibraryKind::Hsm, "hsm", true},
    {LibraryKind::HsmThreaded, "hsm_threaded", true},
    {LibraryKind::Hsmcpp, "hsmcpp", kHasHsmcpp},
    {LibraryKind::BoostMsm, "boost_msm", kHasBoostMsm},
    {LibraryKind::Qp, "qp", kHasQp},
    {LibraryKind::Sml, "sml", kHasSml},
    {LibraryKind::Hfsm2, "hfsm2", kHasHfsm2},
    {LibraryKind::VanillaSwitch, "vanilla_switch", true},
    {LibraryKind::VanillaFp, "vanilla_fp", true},
};

static std::optional<LibraryKind> parse_library_kind(std::string_view name) {
  for (const auto &lib : kLibraries) {
    if (name == lib.name) {
      return lib.kind;
    }
  }
  return std::nullopt;
}

static bool library_available(LibraryKind kind) {
  for (const auto &lib : kLibraries) {
    if (lib.kind == kind) {
      return lib.available;
    }
  }
  return false;
}

static std::unordered_set<LibraryKind> parse_libs_filter(const GlobalConfig &cfg) {
  std::unordered_set<LibraryKind> result;
  if (cfg.libs_filter.empty()) {
    // No filter: include all libraries that are available.
    for (const auto &lib : kLibraries) {
      if (lib.available) {
        result.insert(lib.kind);
      }
    }
    return result;
  }

  std::string_view sv(cfg.libs_filter);
  while (!sv.empty()) {
    auto pos = sv.find(',');
    std::string_view token = pos == std::string_view::npos ? sv : sv.substr(0, pos);
    if (!token.empty()) {
      if (token == "all") {
        result.clear();
        for (const auto &lib : kLibraries) {
          if (lib.available) {
            result.insert(lib.kind);
          }
        }
        return result;
      }
      if (auto kind = parse_library_kind(token)) {
        if (library_available(*kind)) {
          result.insert(*kind);
        } else {
          std::cerr << "Requested library '" << token
                    << "' is not available in this build and will be skipped.\n";
        }
      } else {
        std::cerr << "Unknown library in --libs: '" << token << "' (ignored).\n";
      }
    }
    if (pos == std::string_view::npos) break;
    sv.remove_prefix(pos + 1);
  }

  if (result.empty()) {
    std::cerr << "No usable libraries selected via --libs; falling back to all"
                 " available backends.\n";
    for (const auto &lib : kLibraries) {
      if (lib.available) {
        result.insert(lib.kind);
      }
    }
  }

  return result;
}

// -----------------------------------------------------------------------------
// Result representation
// -----------------------------------------------------------------------------

struct BenchmarkRow {
  std::string scenario;               // ScenarioSpec::name
  std::string library;                // library_name(kind)
  std::size_t iterations{};           // measured iterations
  std::size_t dispatches_per_iteration{};
  double transitions_per_dispatch{};  // from ScenarioSpec

  double dispatches_per_second{};     // measured via measure_throughput
  double transitions_per_second{};    // dispatches_per_second * transitions_per_dispatch
  double percent_vs_baseline{};       // relative to baseline for this scenario
};

// -----------------------------------------------------------------------------
// hsm backend implementations
// -----------------------------------------------------------------------------

namespace backend_hsm {

using namespace hsm;

struct BenchInstance {
  bool guard_flag = false;  // used in guarded scenario
};

// Events shared across scenarios
struct E1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct E2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct G  : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
// Traffic-light tick event for the complex scenario.
struct T  : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};

// Ping-Pong: A <-> B on E1/E2, no entry/exit/guards.
// static void inc_counter(Signal &, BenchInstance &i, const EventBase &) {
//   i.work_counter++;
// }

static constexpr auto model_ping_pong = define(
    "PingPong",
    state("A"),
    state("B"),
    transition(on<E1>(), source("/PingPong/A"), target("/PingPong/B")),
    transition(on<E2>(), source("/PingPong/B"), target("/PingPong/A")),
    initial(target("/PingPong/A")));

// Hierarchical: parent P with children C1, C2 and entry/exit on parent and
// children where supported. Toggle C1 <-> C2 with E1/E2.
static void noop_behavior(Signal &, BenchInstance &, const EventBase &) {}

// Traffic light entry/exit to exercise deeper hierarchies.
static void traffic_entry(Signal &, BenchInstance &, const EventBase &) {}

static void traffic_exit(Signal &, BenchInstance &, const EventBase &) {}

static constexpr auto model_hierarchical = define(
    "Hierarchical",
    state("P", entry(noop_behavior), exit(noop_behavior),
          state("C1", entry(noop_behavior), exit(noop_behavior)),
          state("C2", entry(noop_behavior), exit(noop_behavior)),
          initial(target("/Hierarchical/P/C1")),
          transition(on<E1>(), source("/Hierarchical/P/C1"),
                     target("/Hierarchical/P/C2")),
          transition(on<E2>(), source("/Hierarchical/P/C2"),
                     target("/Hierarchical/P/C1"))),
    initial(target("/Hierarchical/P")));

// Deep hierarchy: L1/L2/{L3a,L3b} with toggling between L3a and L3b.
static constexpr auto model_deep = define(
    "Deep",
    state("L1", entry(noop_behavior), exit(noop_behavior),
          state("L2", entry(noop_behavior), exit(noop_behavior),
                state("L3a", entry(noop_behavior), exit(noop_behavior)),
                state("L3b", entry(noop_behavior), exit(noop_behavior)),
                initial(target("/Deep/L1/L2/L3a")),
                transition(on<E1>(), source("/Deep/L1/L2/L3a"),
                           target("/Deep/L1/L2/L3b")),
                transition(on<E2>(), source("/Deep/L1/L2/L3b"),
                           target("/Deep/L1/L2/L3a"))),
          initial(target("/Deep/L1/L2"))),
    initial(target("/Deep/L1")));

// Guarded transition: on every G event, a guard toggles a flag; only when the
// flag is true do we perform a self-transition on state S. This yields, on
// average, 0.5 transitions per dispatch.
static constexpr auto model_guarded = define(
    "Guarded",
    state("S",
          transition(
              on<G>(),
              guard([](BenchInstance &inst, const G &) {
                // Toggle on every guard evaluation; transition when true.
                inst.guard_flag = !inst.guard_flag;
                return inst.guard_flag;
              }),
              target("/Guarded/S"))),
    initial(target("/Guarded/S")));

// Traffic light controller: complex hierarchical model with an "Operational"
// mode containing nested phases for north/south (NS) and east/west (EW)
// directions plus all-red interlocks. A single tick event advances through a
// six-phase ring:
//   NS_Green -> NS_Yellow -> AllRed1 -> EW_Green -> EW_Yellow -> AllRed2 -> NS_Green
// States are nested to exercise deeper entry/exit paths.
// Transitions use source() DSL at the Operational parent level.
static constexpr auto model_traffic = define(
    "Traffic",
    state("Operational",
          initial(target("/Traffic/Operational/NS")),
          state("NS",
                state("Green", entry(traffic_entry), exit(traffic_exit)),
                state("Yellow", entry(traffic_entry), exit(traffic_exit)),
                initial(target("/Traffic/Operational/NS/Green"))),
          state("EW",
                state("Green", entry(traffic_entry), exit(traffic_exit)),
                state("Yellow", entry(traffic_entry), exit(traffic_exit)),
                initial(target("/Traffic/Operational/EW/Green"))),
          state("AllRed1", entry(traffic_entry), exit(traffic_exit)),
          state("AllRed2", entry(traffic_entry), exit(traffic_exit)),
          transition(on<T>(), source("/Traffic/Operational/NS/Green"),
                     target("/Traffic/Operational/NS/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/NS/Yellow"),
                     target("/Traffic/Operational/AllRed1")),
          transition(on<T>(), source("/Traffic/Operational/AllRed1"),
                     target("/Traffic/Operational/EW/Green")),
          transition(on<T>(), source("/Traffic/Operational/EW/Green"),
                     target("/Traffic/Operational/EW/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/EW/Yellow"),
                     target("/Traffic/Operational/AllRed2")),
          transition(on<T>(), source("/Traffic/Operational/AllRed2"),
                     target("/Traffic/Operational/NS/Green"))),
    initial(target("/Traffic/Operational")));

// CRTP machines for each hsm benchmark model
struct PingPongSM : BenchInstance, HSM<model_ping_pong, PingPongSM> {};
struct HierarchicalSM : BenchInstance, HSM<model_hierarchical, HierarchicalSM> {};
struct DeepSM : BenchInstance, HSM<model_deep, DeepSM> {};
struct GuardedSM : BenchInstance, HSM<model_guarded, GuardedSM> {};
struct TrafficSM : BenchInstance, HSM<model_traffic, TrafficSM> {};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPongSM sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.template process<E1>();
    sm.template process<E2>();
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Use the result to prevent DCE of the entire benchmark function
  volatile std::size_t sink = PingPongSM::event_index<E1::kind>();
  (void)sink;

  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  HierarchicalSM sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.template process<E1>();
    sm.template process<E2>();
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Use the result to prevent DCE
  volatile std::size_t sink = HierarchicalSM::event_index<E1::kind>();
  (void)sink;

  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  DeepSM sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.template process<E1>();
    sm.template process<E2>();
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Use the result to prevent DCE
  volatile std::size_t sink = DeepSM::event_index<E1::kind>();
  (void)sink;

  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  GuardedSM sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.template process<G>();
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Use the result to prevent DCE
  volatile std::size_t sink = GuardedSM::event_index<G::kind>();
  (void)sink;

  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  TrafficSM sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.template process<T>();
    sm.template process<T>();
    sm.template process<T>();
    sm.template process<T>();
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Use the result to prevent DCE
  volatile std::size_t sink = TrafficSM::event_index<T::kind>();
  (void)sink;

  return row;
}

// Threaded version: engine runs in background thread with start().await(),
// dispatches are fire-and-forget without manual task.resume().
static BenchmarkRow run_traffic_light_threaded(const GlobalConfig &cfg,
                                               const ScenarioSpec &scenario) {
  // Heap-allocate SM so it outlives the function scope when thread is detached.
  // Intentionally leaked to avoid use-after-free when the engine thread
  // continues running after we return.
  auto* sm = new TrafficSM();

  // Engine thread: runs the state machine event loop
  std::thread engine_thread([sm]() {
    sm->start().await();
  });

  // Give the engine thread time to start and enter await
  std::this_thread::sleep_for(std::chrono::milliseconds(1));

  auto loop = [sm]() noexcept {
    sm->template dispatch<T>();
    sm->template dispatch<T>();
    sm->template dispatch<T>();
    sm->template dispatch<T>();
    doNotOptimize(*sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsm_threaded";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;

  // Engine thread has no graceful shutdown; detach and let SM + thread
  // continue running (will be cleaned up at process exit)
  engine_thread.detach();

  // Use the result to prevent DCE
  volatile std::size_t sink = TrafficSM::event_index<T::kind>();
  (void)sink;

  return row;
}

}  // namespace backend_hsm

// -----------------------------------------------------------------------------
// Vanilla C++ baselines (switch-based and function-pointer FSMs)
// -----------------------------------------------------------------------------

namespace backend_vanilla_switch {

enum class PingState { A, B };
enum class PingEvent { E1, E2 };

struct PingPongFSM {
  PingState state{PingState::A};

  void dispatch(PingEvent ev) noexcept {
    switch (state) {
      case PingState::A:
        if (ev == PingEvent::E1) {
          state = PingState::B;
        }
        break;
      case PingState::B:
        if (ev == PingEvent::E2) {
          state = PingState::A;
        }
        break;
    }
  }
};

// Hierarchical: we model parent/child implicitly via a combined enum; entry/exit
// are modelled as counters but are not touched in the hot path to keep the
// baseline comparable (no logging, no allocation).

enum class HierState { P_C1, P_C2 };
enum class HierEvent { ToC2, ToC1 };

struct HierarchicalFSM {
  HierState state{HierState::P_C1};

  void dispatch(HierEvent ev) noexcept {
    switch (state) {
      case HierState::P_C1:
        if (ev == HierEvent::ToC2) {
          state = HierState::P_C2;
        }
        break;
      case HierState::P_C2:
        if (ev == HierEvent::ToC1) {
          state = HierState::P_C1;
        }
        break;
    }
  }
};

enum class DeepState { L3a, L3b };
enum class DeepEvent { ToB, ToA };

struct DeepFSM {
  DeepState state{DeepState::L3a};

  void dispatch(DeepEvent ev) noexcept {
    switch (state) {
      case DeepState::L3a:
        if (ev == DeepEvent::ToB) {
          state = DeepState::L3b;
        }
        break;
      case DeepState::L3b:
        if (ev == DeepEvent::ToA) {
          state = DeepState::L3a;
        }
        break;
    }
  }
};

enum class GuardEvent { Tick };

enum class GuardState { S };

struct GuardedFSM {
  GuardState state{GuardState::S};
  bool flag = false;

  void dispatch(GuardEvent) noexcept {
    flag = !flag;
    if (flag) {
      // conceptual self-transition; we keep it trivial
      state = GuardState::S;
    }
  }
};

enum class TrafficState {
  NS_Green,
  NS_Yellow,
  AllRed1,
  EW_Green,
  EW_Yellow,
  AllRed2,
};

enum class TrafficEvent { Tick };

struct TrafficFSM {
  TrafficState state{TrafficState::NS_Green};

  void dispatch(TrafficEvent) noexcept {
    switch (state) {
      case TrafficState::NS_Green:
        state = TrafficState::NS_Yellow;
        break;
      case TrafficState::NS_Yellow:
        state = TrafficState::AllRed1;
        break;
      case TrafficState::AllRed1:
        state = TrafficState::EW_Green;
        break;
      case TrafficState::EW_Green:
        state = TrafficState::EW_Yellow;
        break;
      case TrafficState::EW_Yellow:
        state = TrafficState::AllRed2;
        break;
      case TrafficState::AllRed2:
        state = TrafficState::NS_Green;
        break;
    }
  }
};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPongFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(PingEvent::E1);
    fsm.dispatch(PingEvent::E2);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_switch";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  // doNotOptimize(fsm); // optimization prevented inside measure_throughput
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  HierarchicalFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(HierEvent::ToC2);
    fsm.dispatch(HierEvent::ToC1);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_switch";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  // doNotOptimize(fsm);
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  DeepFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(DeepEvent::ToB);
    fsm.dispatch(DeepEvent::ToA);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_switch";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  // doNotOptimize(fsm);
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  GuardedFSM fsm;

  auto loop = [&]() noexcept { 
    fsm.dispatch(GuardEvent::Tick);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_switch";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  // doNotOptimize(fsm);
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  TrafficFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_switch";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  // doNotOptimize(fsm);
  return row;
}

}  // namespace backend_vanilla_switch

namespace backend_vanilla_fp {

// Function-pointer based FSMs for the same scenarios. For simplicity and
// performance, events are represented as small enums or integers.

enum class PingEvent { E1, E2 };

struct PingPongFSM {
  using StateFn = void (PingPongFSM::*)(PingEvent) noexcept;

  StateFn state;

  PingPongFSM() noexcept : state(&PingPongFSM::stateA) {}

  void dispatch(PingEvent ev) noexcept { (this->*state)(ev); }

  void stateA(PingEvent ev) noexcept {
    if (ev == PingEvent::E1) {
      state = &PingPongFSM::stateB;
    }
  }

  void stateB(PingEvent ev) noexcept {
    if (ev == PingEvent::E2) {
      state = &PingPongFSM::stateA;
    }
  }
};

enum class HierEvent { ToC2, ToC1 };

struct HierarchicalFSM {
  using StateFn = void (HierarchicalFSM::*)(HierEvent) noexcept;

  StateFn state;

  HierarchicalFSM() noexcept : state(&HierarchicalFSM::stateC1) {}

  void dispatch(HierEvent ev) noexcept { (this->*state)(ev); }

  void stateC1(HierEvent ev) noexcept {
    if (ev == HierEvent::ToC2) {
      state = &HierarchicalFSM::stateC2;
    }
  }

  void stateC2(HierEvent ev) noexcept {
    if (ev == HierEvent::ToC1) {
      state = &HierarchicalFSM::stateC1;
    }
  }
};

enum class DeepEvent { ToB, ToA };

struct DeepFSM {
  using StateFn = void (DeepFSM::*)(DeepEvent) noexcept;

  StateFn state;

  DeepFSM() noexcept : state(&DeepFSM::stateL3a) {}

  void dispatch(DeepEvent ev) noexcept { (this->*state)(ev); }

  void stateL3a(DeepEvent ev) noexcept {
    if (ev == DeepEvent::ToB) {
      state = &DeepFSM::stateL3b;
    }
  }

  void stateL3b(DeepEvent ev) noexcept {
    if (ev == DeepEvent::ToA) {
      state = &DeepFSM::stateL3a;
    }
  }
};

enum class GuardEvent { Tick };

struct GuardedFSM {
  using StateFn = void (GuardedFSM::*)(GuardEvent) noexcept;

  StateFn state;
  bool flag = false;

  GuardedFSM() noexcept : state(&GuardedFSM::stateS) {}

  void dispatch(GuardEvent ev) noexcept { (this->*state)(ev); }

  void stateS(GuardEvent) noexcept {
    flag = !flag;
    if (flag) {
      // conceptual self-transition
      state = &GuardedFSM::stateS;
    }
  }
};

enum class TrafficEvent { Tick };

struct TrafficFSM {
  using StateFn = void (TrafficFSM::*)(TrafficEvent) noexcept;

  StateFn state;

  TrafficFSM() noexcept : state(&TrafficFSM::stateNSGreen) {}

  void dispatch(TrafficEvent ev) noexcept { (this->*state)(ev); }

  void stateNSGreen(TrafficEvent) noexcept {
    state = &TrafficFSM::stateNSYellow;
  }

  void stateNSYellow(TrafficEvent) noexcept {
    state = &TrafficFSM::stateAllRed1;
  }

  void stateAllRed1(TrafficEvent) noexcept {
    state = &TrafficFSM::stateEWGreen;
  }

  void stateEWGreen(TrafficEvent) noexcept {
    state = &TrafficFSM::stateEWYellow;
  }

  void stateEWYellow(TrafficEvent) noexcept {
    state = &TrafficFSM::stateAllRed2;
  }

  void stateAllRed2(TrafficEvent) noexcept {
    state = &TrafficFSM::stateNSGreen;
  }
};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPongFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(PingEvent::E1);
    fsm.dispatch(PingEvent::E2);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_fp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  HierarchicalFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(HierEvent::ToC2);
    fsm.dispatch(HierEvent::ToC1);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_fp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  DeepFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(DeepEvent::ToB);
    fsm.dispatch(DeepEvent::ToA);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_fp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  GuardedFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(GuardEvent::Tick);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_fp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  TrafficFSM fsm;

  auto loop = [&]() noexcept {
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    fsm.dispatch(TrafficEvent::Tick);
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "vanilla_fp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

}  // namespace backend_vanilla_fp

// -----------------------------------------------------------------------------
// Boost-ext.SML backend (conditional)
// -----------------------------------------------------------------------------

#if defined(HSM_BENCH_ENABLE_SML)
namespace backend_sml {

namespace sml = boost::sml;

// Events shared across scenarios
struct E1 {
};
struct E2 {
};
struct G {
};
struct T {
};

struct guard_data {
  bool flag{false};
};

// 1) Simple Ping-Pong: A <-> B on E1/E2.
struct ping_pong_sm {
  auto operator()() const {
    using namespace sml;
    // Two simple states A and B with bidirectional transitions.
    return make_transition_table(
        *"A"_s + event<E1> = "B"_s,
         "B"_s + event<E2> = "A"_s);
  }
};

// 2) Hierarchical Parent/Child (P/C1/C2). SML does not model hierarchy in the
// same way as hsm, but for throughput purposes we mirror the same number of
// logical states and transitions.
struct hierarchical_sm {
  auto operator()() const {
    using namespace sml;
    // States P_C1 and P_C2 represent parent P with children C1 and C2.
    return make_transition_table(
        *"P_C1"_s + event<E1> = "P_C2"_s,
         "P_C2"_s + event<E2> = "P_C1"_s);
  }
};

// 3) Deep hierarchy: L1/L2/{L3a,L3b}. Again we flatten the representation but
// keep the same number of states and transitions.
struct deep_sm {
  auto operator()() const {
    using namespace sml;
    return make_transition_table(
        *"L3a"_s + event<E1> = "L3b"_s,
         "L3b"_s + event<E2> = "L3a"_s);
  }
};

// 4) Guarded transition: every other dispatch performs a self-transition.
struct guarded_sm {
  auto operator()() const {
    using namespace sml;
    const auto toggle_guard = [](guard_data &d) {
      d.flag = !d.flag;
      return d.flag;  // true on every other call
    };

    return make_transition_table(
        *"S"_s + event<G> [toggle_guard] = "S"_s);
  }
};

// 5) Traffic light: six-state ring driven by T.
// NS_Green -> NS_Yellow -> AllRed1 -> EW_Green -> EW_Yellow -> AllRed2 -> NS_Green.
struct traffic_sm {
  auto operator()() const {
    using namespace sml;
    return make_transition_table(
        *"NS_Green"_s + event<T> = "NS_Yellow"_s,
         "NS_Yellow"_s + event<T> = "AllRed1"_s,
         "AllRed1"_s + event<T> = "EW_Green"_s,
         "EW_Green"_s + event<T> = "EW_Yellow"_s,
         "EW_Yellow"_s + event<T> = "AllRed2"_s,
         "AllRed2"_s + event<T> = "NS_Green"_s);
  }
};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  guard_data data{};  // unused here, but keep interface consistent
  sml::sm<ping_pong_sm> sm{data};

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "sml";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  doNotOptimize(sm);
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  guard_data data{};
  sml::sm<hierarchical_sm> sm{data};

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "sml";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  doNotOptimize(sm);
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  guard_data data{};
  sml::sm<deep_sm> sm{data};

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "sml";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  doNotOptimize(sm);
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  guard_data data{};
  sml::sm<guarded_sm> sm{data};

  auto loop = [&]() noexcept { 
    sm.process_event(G{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "sml";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  doNotOptimize(sm);
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  sml::sm<traffic_sm> sm;  // no external data required

  auto loop = [&]() noexcept {
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "sml";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  doNotOptimize(sm);
  return row;
}

}  // namespace backend_sml
#endif  // defined(HSM_BENCH_ENABLE_SML)

// -----------------------------------------------------------------------------
// hsmcpp backend (conditional)
// -----------------------------------------------------------------------------

#if defined(HSM_BENCH_ENABLE_HSMCPP)
namespace backend_hsmcpp {

// We use integer state and event identifiers mapped from enums so we can work
// with the existing ID-based hsmcpp HierarchicalStateMachine API.

enum class StateId : int {
  A,
  B,
  P_C1,
  P_C2,
  L3a,
  L3b,
  S,
  TL_NS_G,
  TL_NS_Y,
  TL_AR1,
  TL_EW_G,
  TL_EW_Y,
  TL_AR2,
};

enum class EventId : int {
  E1,
  E2,
  G,
  T,
};

using Hsm = hsmcpp::HierarchicalStateMachine;

static inline hsmcpp::StateID_t sid(StateId s) noexcept {
  return static_cast<hsmcpp::StateID_t>(s);
}

static inline hsmcpp::EventID_t eid(EventId e) noexcept {
  return static_cast<hsmcpp::EventID_t>(e);
}

// Ping-Pong
static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  auto dispatcher = hsmcpp::HsmEventDispatcherSTD::create();
  Hsm hsm(sid(StateId::A));

  hsm.registerState(sid(StateId::A));
  hsm.registerState(sid(StateId::B));

  hsm.registerTransition(sid(StateId::A), sid(StateId::B), eid(EventId::E1));
  hsm.registerTransition(sid(StateId::B), sid(StateId::A), eid(EventId::E2));

  hsm.initialize(dispatcher);

  auto loop = [&]() noexcept {
    hsm.transition(eid(EventId::E1));
    hsm.transition(eid(EventId::E2));
    doNotOptimize(hsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsmcpp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

// Hierarchical (flattened P/C1/C2)
static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  auto dispatcher = hsmcpp::HsmEventDispatcherSTD::create();
  Hsm hsm(sid(StateId::P_C1));

  hsm.registerState(sid(StateId::P_C1));
  hsm.registerState(sid(StateId::P_C2));

  hsm.registerTransition(sid(StateId::P_C1), sid(StateId::P_C2), eid(EventId::E1));
  hsm.registerTransition(sid(StateId::P_C2), sid(StateId::P_C1), eid(EventId::E2));

  hsm.initialize(dispatcher);

  auto loop = [&]() noexcept {
    hsm.transition(eid(EventId::E1));
    hsm.transition(eid(EventId::E2));
    doNotOptimize(hsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsmcpp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

// Deep hierarchy (flattened L3a/L3b)
static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  auto dispatcher = hsmcpp::HsmEventDispatcherSTD::create();
  Hsm hsm(sid(StateId::L3a));

  hsm.registerState(sid(StateId::L3a));
  hsm.registerState(sid(StateId::L3b));

  hsm.registerTransition(sid(StateId::L3a), sid(StateId::L3b), eid(EventId::E1));
  hsm.registerTransition(sid(StateId::L3b), sid(StateId::L3a), eid(EventId::E2));

  hsm.initialize(dispatcher);

  auto loop = [&]() noexcept {
    hsm.transition(eid(EventId::E1));
    hsm.transition(eid(EventId::E2));
    doNotOptimize(hsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsmcpp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

// Guarded transition: every other dispatch performs a self-transition.
static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  auto dispatcher = hsmcpp::HsmEventDispatcherSTD::create();
  Hsm hsm(sid(StateId::S));

  hsm.registerState(sid(StateId::S));
  hsm.registerTransition(sid(StateId::S), sid(StateId::S), eid(EventId::G));

  hsm.initialize(dispatcher);

  bool guard_flag = false;

  auto loop = [&]() noexcept {
    // Emulate a guard in the benchmark harness: only fire the event on every
    // other dispatch so the effective transitions/dispatch match the scenario.
    guard_flag = !guard_flag;
    if (guard_flag) {
      hsm.transition(eid(EventId::G));
    }
    doNotOptimize(hsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsmcpp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

// Traffic light controller (flattened six-state ring) driven by T.
static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  auto dispatcher = hsmcpp::HsmEventDispatcherSTD::create();
  Hsm hsm(sid(StateId::TL_NS_G));

  hsm.registerState(sid(StateId::TL_NS_G));
  hsm.registerState(sid(StateId::TL_NS_Y));
  hsm.registerState(sid(StateId::TL_AR1));
  hsm.registerState(sid(StateId::TL_EW_G));
  hsm.registerState(sid(StateId::TL_EW_Y));
  hsm.registerState(sid(StateId::TL_AR2));

  hsm.registerTransition(sid(StateId::TL_NS_G), sid(StateId::TL_NS_Y),
                         eid(EventId::T));
  hsm.registerTransition(sid(StateId::TL_NS_Y), sid(StateId::TL_AR1),
                         eid(EventId::T));
  hsm.registerTransition(sid(StateId::TL_AR1), sid(StateId::TL_EW_G),
                         eid(EventId::T));
  hsm.registerTransition(sid(StateId::TL_EW_G), sid(StateId::TL_EW_Y),
                         eid(EventId::T));
  hsm.registerTransition(sid(StateId::TL_EW_Y), sid(StateId::TL_AR2),
                         eid(EventId::T));
  hsm.registerTransition(sid(StateId::TL_AR2), sid(StateId::TL_NS_G),
                         eid(EventId::T));

  hsm.initialize(dispatcher);

  auto loop = [&]() noexcept {
    hsm.transition(eid(EventId::T));
    hsm.transition(eid(EventId::T));
    hsm.transition(eid(EventId::T));
    hsm.transition(eid(EventId::T));
    doNotOptimize(hsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hsmcpp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

}  // namespace backend_hsmcpp
#endif  // defined(HSM_BENCH_ENABLE_HSMCPP)

// -----------------------------------------------------------------------------
// Boost.MSM backend (conditional)
// -----------------------------------------------------------------------------

#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
namespace backend_boost_msm {

namespace msm = boost::msm;
namespace mpl = boost::mpl;
using namespace msm::front;  // for Row, state<>

struct E1 {};
struct E2 {};
struct G  {};
struct T  {};

// Entry/exit hooks for the traffic light scenario are intentionally left
// as no-ops to keep per-transition work consistent across backends.

// Ping-Pong machine
struct ping_pong_ : state_machine_def<ping_pong_> {
  struct A : state<> {};
  struct B : state<> {};

  using initial_state = A;

  struct transition_table
      : mpl::vector<
            //    Start  Event Next
            Row<A, E1, B>,
            Row<B, E2, A>> {};
};

using PingPong = msm::back::state_machine<ping_pong_>;

// Hierarchical (flattened P_C1/P_C2)
struct hierarchical_ : state_machine_def<hierarchical_> {
  struct P_C1 : state<> {};
  struct P_C2 : state<> {};

  using initial_state = P_C1;

  struct transition_table
      : mpl::vector<
            Row<P_C1, E1, P_C2>,
            Row<P_C2, E2, P_C1>> {};
};

using Hierarchical = msm::back::state_machine<hierarchical_>;

// Deep (flattened L3a/L3b)
struct deep_ : state_machine_def<deep_> {
  struct L3a : state<> {};
  struct L3b : state<> {};

  using initial_state = L3a;

  struct transition_table
      : mpl::vector<
            Row<L3a, E1, L3b>,
            Row<L3b, E2, L3a>> {};
};

using Deep = msm::back::state_machine<deep_>;

// Guarded: every other dispatch triggers self-transition
struct guarded_ : state_machine_def<guarded_> {
  struct S : state<> {};

  using initial_state = S;

  bool flag{false};

  struct toggle_guard {
    template <class EVT, class FSM, class SourceState, class TargetState>
    bool operator()(EVT const &, FSM &fsm, SourceState &, TargetState &) const {
      fsm.flag = !fsm.flag;
      return fsm.flag;
    }
  };

  struct transition_table
      : mpl::vector<
            Row<S, G, S, toggle_guard>> {};
};

using Guarded = msm::back::state_machine<guarded_>;

// Traffic light: six-state ring driven by T. Each leaf state performs simple
// entry/exit work (incrementing counters) to better mirror hsm's traffic
// model, which exercises entry/exit behaviors on each phase change.
//
// We model this with a dedicated "Operational" submachine containing the six
// phases, and a root "Traffic" machine whose only active child is the
// Operational submachine. This roughly corresponds to the
// /Traffic/Operational/... structure used in the hsm model.
struct operational_ : state_machine_def<operational_> {
  struct NS_Green : state<> {};

  struct NS_Yellow : state<> {};

  struct AllRed1 : state<> {};

  struct EW_Green : state<> {};

  struct EW_Yellow : state<> {};

  struct AllRed2 : state<> {};

  using initial_state = NS_Green;

  struct transition_table
      : mpl::vector<
            Row<NS_Green,  T, NS_Yellow>,
            Row<NS_Yellow, T, AllRed1>,
            Row<AllRed1,   T, EW_Green>,
            Row<EW_Green,  T, EW_Yellow>,
            Row<EW_Yellow, T, AllRed2>,
            Row<AllRed2,   T, NS_Green>> {};
};

// Traffic machine corresponding directly to the /Traffic/Operational composite
// in the hsm model. We model a single state machine with six phases rather
// than an extra wrapper machine layer.
using Traffic = msm::back::state_machine<operational_>;

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPong sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "boost_msm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  Hierarchical sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "boost_msm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  Deep sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.process_event(E1{});
    sm.process_event(E2{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "boost_msm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  Guarded sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.process_event(G{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "boost_msm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  Traffic sm;
  sm.start();

  auto loop = [&]() noexcept {
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    sm.process_event(T{});
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "boost_msm";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

}  // namespace backend_boost_msm
#endif  // defined(HSM_BENCH_ENABLE_BOOST_MSM)

// -----------------------------------------------------------------------------
// HFSM2 backend (conditional)
// -----------------------------------------------------------------------------

#if defined(HSM_BENCH_ENABLE_HFSM2)
namespace backend_hfsm2 {

namespace hfsm = hfsm2;

struct E1 {
};
struct E2 {
};
struct G  {
};
struct T  {
};

using Config = hfsm::Config;
using M = hfsm::MachineT<Config>;

// Ping-Pong FSM: A <-> B on E1/E2
struct PingPongFsm {
  struct A;
  struct B;

  using FSM = M::PeerRoot<A, B>;

  struct A : FSM::State {
    void update(FullControl &control) {
      control.changeTo<B>();
    }
  };

  struct B : FSM::State {
    void update(FullControl &control) {
      control.changeTo<A>();
    }
  };
};

// Hierarchical (flattened P_C1/P_C2)
struct HierFsm {
  struct P_C1;
  struct P_C2;

  using FSM = M::PeerRoot<P_C1, P_C2>;

  struct P_C1 : FSM::State {
    void update(FullControl &control) {
      control.changeTo<P_C2>();
    }
  };

  struct P_C2 : FSM::State {
    void update(FullControl &control) {
      control.changeTo<P_C1>();
    }
  };
};

// Deep (flattened L3a/L3b)
struct DeepFsm {
  struct L3a;
  struct L3b;

  using FSM = M::PeerRoot<L3a, L3b>;

  struct L3a : FSM::State {
    void update(FullControl &control) {
      control.changeTo<L3b>();
    }
  };

  struct L3b : FSM::State {
    void update(FullControl &control) {
      control.changeTo<L3a>();
    }
  };
};

// Guarded: state S with every other dispatch causing self-transition.
struct GuardedFsm {
  struct S;

  using FSM = M::PeerRoot<S>;

  struct S : FSM::State {
    void update(FullControl &control) {
      // Always perform a self-transition when invoked; the harness controls
      // how often update() is called to emulate a 0.5 transitions/dispatch
      // guarded scenario.
      control.changeTo<S>();
    }
  };
};

// Traffic light: six-state ring advanced via update() calls.
struct TrafficFsm {
  struct NS_Green;
  struct NS_Yellow;
  struct AllRed1;
  struct EW_Green;
  struct EW_Yellow;
  struct AllRed2;

  using FSM = M::PeerRoot<NS_Green, NS_Yellow, AllRed1,
                          EW_Green, EW_Yellow, AllRed2>;

  struct NS_Green : FSM::State {
    void update(FullControl &control) {
      control.changeTo<NS_Yellow>();
    }
  };

  struct NS_Yellow : FSM::State {
    void update(FullControl &control) {
      control.changeTo<AllRed1>();
    }
  };

  struct AllRed1 : FSM::State {
    void update(FullControl &control) {
      control.changeTo<EW_Green>();
    }
  };

  struct EW_Green : FSM::State {
    void update(FullControl &control) {
      control.changeTo<EW_Yellow>();
    }
  };

  struct EW_Yellow : FSM::State {
    void update(FullControl &control) {
      control.changeTo<AllRed2>();
    }
  };

  struct AllRed2 : FSM::State {
    void update(FullControl &control) {
      control.changeTo<NS_Green>();
    }
  };
};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPongFsm::FSM::Instance fsm;

  auto loop = [&]() noexcept {
    fsm.update();
    fsm.update();
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hfsm2";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  HierFsm::FSM::Instance fsm;

  auto loop = [&]() noexcept {
    fsm.update();
    fsm.update();
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hfsm2";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  DeepFsm::FSM::Instance fsm;

  auto loop = [&]() noexcept {
    fsm.update();
    fsm.update();
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hfsm2";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  GuardedFsm::FSM::Instance fsm;

  bool flag = false;

  auto loop = [&]() noexcept {
    flag = !flag;
    if (flag) {
      fsm.update();
    }
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hfsm2";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  TrafficFsm::FSM::Instance fsm;

  auto loop = [&]() noexcept {
    fsm.update();
    fsm.update();
    fsm.update();
    fsm.update();
    doNotOptimize(fsm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "hfsm2";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

}  // namespace backend_hfsm2
#endif  // defined(HSM_BENCH_ENABLE_HFSM2)

// -----------------------------------------------------------------------------
// QP/C++ backend (conditional, minimal ping-pong only)
// -----------------------------------------------------------------------------

#if defined(HSM_BENCH_ENABLE_QP)
namespace QP {
namespace QF {
void enterCriticalSection_() {}
void leaveCriticalSection_() {}
}  // namespace QF
}  // namespace QP

namespace backend_qp {

extern "C" void Q_onError(char const * const module, int const id) {
  (void)module;
  (void)id;
  std::cerr << "QP Assertion Failure: " << (module ? module : "?") << ":" << id << "\n";
  std::terminate();
}

// We use a very small QHsm-derived ping-pong machine. Only the Ping-Pong
// scenario is implemented for QP, as QP integration and licensing are
// significantly heavier than the header-only libraries.

struct PingEvt : public QP::QEvt {
  PingEvt(QP::QSignal s) : QP::QEvt(s) {}
};

enum Signals {
    E1_SIG = QP::Q_USER_SIG,
    E2_SIG,
    G_SIG,
    T_SIG
};

class PingPongHsm : public QP::QHsm {
 public:
  PingPongHsm() noexcept : QP::QHsm(&initial) {}

 private:
  static QP::QState initial(void * const me_void, QP::QEvt const * const e) {
    PingPongHsm *me = static_cast<PingPongHsm *>(me_void);
    (void)e;
    return Q_TRAN(&PingPongHsm::stateA);
  }

  static QP::QState stateA(void * const me_void, QP::QEvt const * const e) {
    PingPongHsm *me = static_cast<PingPongHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E1_SIG:
        return Q_TRAN(&PingPongHsm::stateB);
      default:
        return Q_SUPER(&PingPongHsm::top);
    }
  }

  static QP::QState stateB(void * const me_void, QP::QEvt const * const e) {
    PingPongHsm *me = static_cast<PingPongHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E2_SIG:
        return Q_TRAN(&PingPongHsm::stateA);
      default:
        return Q_SUPER(&PingPongHsm::top);
    }
  }
};

// Hierarchical
class HierarchicalHsm : public QP::QHsm {
 public:
  HierarchicalHsm() noexcept : QP::QHsm(&initial) {}

 private:
  static QP::QState initial(void * const me_void, QP::QEvt const * const e) {
    HierarchicalHsm *me = static_cast<HierarchicalHsm *>(me_void);
    (void)e;
    return Q_TRAN(&HierarchicalHsm::P);
  }

  static QP::QState P(void * const me_void, QP::QEvt const * const e) {
    HierarchicalHsm *me = static_cast<HierarchicalHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case Q_INIT_SIG:
        return Q_TRAN(&HierarchicalHsm::C1);
      default:
        return Q_SUPER(&HierarchicalHsm::top);
    }
  }

  static QP::QState C1(void * const me_void, QP::QEvt const * const e) {
    HierarchicalHsm *me = static_cast<HierarchicalHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E1_SIG:
        return Q_TRAN(&HierarchicalHsm::C2);
      default:
        return Q_SUPER(&HierarchicalHsm::P);
    }
  }

  static QP::QState C2(void * const me_void, QP::QEvt const * const e) {
    HierarchicalHsm *me = static_cast<HierarchicalHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E2_SIG:
        return Q_TRAN(&HierarchicalHsm::C1);
      default:
        return Q_SUPER(&HierarchicalHsm::P);
    }
  }
};

// Deep
class DeepHsm : public QP::QHsm {
 public:
  DeepHsm() noexcept : QP::QHsm(&initial) {}

 private:
  static QP::QState initial(void * const me_void, QP::QEvt const * const e) {
    DeepHsm *me = static_cast<DeepHsm *>(me_void);
    (void)e;
    return Q_TRAN(&DeepHsm::L1);
  }

  static QP::QState L1(void * const me_void, QP::QEvt const * const e) {
    DeepHsm *me = static_cast<DeepHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case Q_INIT_SIG:
        return Q_TRAN(&DeepHsm::L2);
      default:
        return Q_SUPER(&DeepHsm::top);
    }
  }

  static QP::QState L2(void * const me_void, QP::QEvt const * const e) {
    DeepHsm *me = static_cast<DeepHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case Q_INIT_SIG:
        return Q_TRAN(&DeepHsm::L3a);
      default:
        return Q_SUPER(&DeepHsm::L1);
    }
  }

  static QP::QState L3a(void * const me_void, QP::QEvt const * const e) {
    DeepHsm *me = static_cast<DeepHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E1_SIG:
        return Q_TRAN(&DeepHsm::L3b);
      default:
        return Q_SUPER(&DeepHsm::L2);
    }
  }

  static QP::QState L3b(void * const me_void, QP::QEvt const * const e) {
    DeepHsm *me = static_cast<DeepHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case E2_SIG:
        return Q_TRAN(&DeepHsm::L3a);
      default:
        return Q_SUPER(&DeepHsm::L2);
    }
  }
};

// Guarded
class GuardedHsm : public QP::QHsm {
 public:
  GuardedHsm() noexcept : QP::QHsm(&initial) {}
  bool flag = false;

 private:
  static QP::QState initial(void * const me_void, QP::QEvt const * const e) {
    GuardedHsm *me = static_cast<GuardedHsm *>(me_void);
    (void)e;
    return Q_TRAN(&GuardedHsm::S);
  }

  static QP::QState S(void * const me_void, QP::QEvt const * const e) {
    GuardedHsm *me = static_cast<GuardedHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case G_SIG:
        me->flag = !me->flag;
        if (me->flag) {
          return Q_TRAN(&GuardedHsm::S);
        }
        return Q_RET_HANDLED;
      default:
        return Q_SUPER(&GuardedHsm::top);
    }
  }
};

// Traffic
class TrafficLightHsm : public QP::QHsm {
 public:
  TrafficLightHsm() noexcept : QP::QHsm(&initial) {}

 private:
  static QP::QState initial(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    (void)e;
    return Q_TRAN(&TrafficLightHsm::NS_Green);
  }

  static QP::QState NS_Green(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::NS_Yellow);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }

  static QP::QState NS_Yellow(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::AllRed1);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }

  static QP::QState AllRed1(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::EW_Green);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }

  static QP::QState EW_Green(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::EW_Yellow);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }

  static QP::QState EW_Yellow(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::AllRed2);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }

  static QP::QState AllRed2(void * const me_void, QP::QEvt const * const e) {
    TrafficLightHsm *me = static_cast<TrafficLightHsm *>(me_void);
    switch (e->sig) {
      case Q_ENTRY_SIG:
      case Q_EXIT_SIG:
        return Q_RET_HANDLED;
      case T_SIG:
        return Q_TRAN(&TrafficLightHsm::NS_Green);
      default:
        return Q_SUPER(&TrafficLightHsm::top);
    }
  }
};

static BenchmarkRow run_ping_pong(const GlobalConfig &cfg,
                                  const ScenarioSpec &scenario) {
  PingPongHsm sm;
  sm.init(0);

  PingEvt e1{static_cast<QP::QSignal>(E1_SIG)};
  PingEvt e2{static_cast<QP::QSignal>(E2_SIG)};

  auto loop = [&]() noexcept {
    sm.dispatch(&e1, 0);
    sm.dispatch(&e2, 0);
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "qp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_hierarchical(const GlobalConfig &cfg,
                                     const ScenarioSpec &scenario) {
  HierarchicalHsm sm;
  sm.init(0);

  PingEvt e1{static_cast<QP::QSignal>(E1_SIG)};
  PingEvt e2{static_cast<QP::QSignal>(E2_SIG)};

  auto loop = [&]() noexcept {
    sm.dispatch(&e1, 0);
    sm.dispatch(&e2, 0);
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "qp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_deep(const GlobalConfig &cfg,
                             const ScenarioSpec &scenario) {
  DeepHsm sm;
  sm.init(0);

  PingEvt e1{static_cast<QP::QSignal>(E1_SIG)};
  PingEvt e2{static_cast<QP::QSignal>(E2_SIG)};

  auto loop = [&]() noexcept {
    sm.dispatch(&e1, 0);
    sm.dispatch(&e2, 0);
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "qp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_guarded(const GlobalConfig &cfg,
                                const ScenarioSpec &scenario) {
  GuardedHsm sm;
  sm.init(0);

  PingEvt g{static_cast<QP::QSignal>(G_SIG)};

  auto loop = [&]() noexcept {
    sm.dispatch(&g, 0);
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "qp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

static BenchmarkRow run_traffic_light(const GlobalConfig &cfg,
                                      const ScenarioSpec &scenario) {
  TrafficLightHsm sm;
  sm.init(0);

  PingEvt t{static_cast<QP::QSignal>(T_SIG)};

  auto loop = [&]() noexcept {
    sm.dispatch(&t, 0);
    sm.dispatch(&t, 0);
    sm.dispatch(&t, 0);
    sm.dispatch(&t, 0);
    doNotOptimize(sm);
  };

  BenchmarkRow row;
  row.scenario = scenario.name;
  row.library = "qp";
  row.iterations = cfg.iterations;
  row.dispatches_per_iteration = scenario.dispatches_per_iteration;
  row.transitions_per_dispatch = scenario.transitions_per_dispatch;
  row.dispatches_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         scenario.dispatches_per_iteration, loop);
  row.transitions_per_second =
      row.dispatches_per_second * row.transitions_per_dispatch;
  return row;
}

// For the other scenarios, QP is intentionally left unimplemented to keep the
// integration surface small. They will simply not be registered in the
// backend runner table unless extended in the future.

}  // namespace backend_qp
#endif  // defined(HSM_BENCH_ENABLE_QP)

// -----------------------------------------------------------------------------
// Backend registry and scenario dispatch
// -----------------------------------------------------------------------------

struct BackendRunner {
  const ScenarioSpec *scenario;
  LibraryKind library;
  BenchmarkRow (*run)(const GlobalConfig &, const ScenarioSpec &);
};

static std::unordered_set<std::string> parse_scenarios_filter(const GlobalConfig &cfg) {
  std::unordered_set<std::string> result;
  if (cfg.scenario_ids.empty()) {
    return result;
  }
  std::string_view sv(cfg.scenario_ids);
  while (!sv.empty()) {
    auto pos = sv.find(',');
    std::string_view token = pos == std::string_view::npos ? sv : sv.substr(0, pos);
    if (!token.empty()) {
      result.emplace(token);
    }
    if (pos == std::string_view::npos) break;
    sv.remove_prefix(pos + 1);
  }
  return result;
}

static std::vector<BackendRunner> build_runners(const GlobalConfig &cfg) {
  std::vector<BackendRunner> runners;
  runners.reserve(32);

  const auto libs = parse_libs_filter(cfg);
  const auto selected_scenarios = parse_scenarios_filter(cfg);

  auto add = [&](const ScenarioSpec &spec, LibraryKind lib,
                 BenchmarkRow (*fn)(const GlobalConfig &, const ScenarioSpec &)) {
    if (libs.find(lib) == libs.end()) return;
    if (!library_available(lib)) return;
    runners.push_back(BackendRunner{&spec, lib, fn});
  };

  for (const auto &spec : kScenarios) {
    // If explicit scenario ids were provided, they take precedence over the
    // legacy name substring filter.
    if (!selected_scenarios.empty()) {
      if (selected_scenarios.find(spec.id) == selected_scenarios.end()) {
        continue;
      }
    } else if (!cfg.scenario_filter.empty() &&
               std::string_view(spec.name).find(cfg.scenario_filter) ==
                   std::string_view::npos) {
      continue;
    }

    if (std::strcmp(spec.id, "ping_pong") == 0) {
      add(spec, LibraryKind::Hsm, &backend_hsm::run_ping_pong);
      add(spec, LibraryKind::VanillaSwitch, &backend_vanilla_switch::run_ping_pong);
      add(spec, LibraryKind::VanillaFp, &backend_vanilla_fp::run_ping_pong);
#if defined(HSM_BENCH_ENABLE_SML)
      add(spec, LibraryKind::Sml, &backend_sml::run_ping_pong);
#endif
#if defined(HSM_BENCH_ENABLE_HSMCPP)
      add(spec, LibraryKind::Hsmcpp, &backend_hsmcpp::run_ping_pong);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
      add(spec, LibraryKind::BoostMsm, &backend_boost_msm::run_ping_pong);
#endif
#if defined(HSM_BENCH_ENABLE_HFSM2)
      add(spec, LibraryKind::Hfsm2, &backend_hfsm2::run_ping_pong);
#endif
#if defined(HSM_BENCH_ENABLE_QP)
      add(spec, LibraryKind::Qp, &backend_qp::run_ping_pong);
#endif
      // Additional libraries can be added here behind feature flags.
    } else if (std::strcmp(spec.id, "hierarchical") == 0) {
      add(spec, LibraryKind::Hsm, &backend_hsm::run_hierarchical);
      add(spec, LibraryKind::VanillaSwitch,
          &backend_vanilla_switch::run_hierarchical);
      add(spec, LibraryKind::VanillaFp, &backend_vanilla_fp::run_hierarchical);
#if defined(HSM_BENCH_ENABLE_SML)
      add(spec, LibraryKind::Sml, &backend_sml::run_hierarchical);
#endif
#if defined(HSM_BENCH_ENABLE_HSMCPP)
      add(spec, LibraryKind::Hsmcpp, &backend_hsmcpp::run_hierarchical);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
      add(spec, LibraryKind::BoostMsm, &backend_boost_msm::run_hierarchical);
#endif
#if defined(HSM_BENCH_ENABLE_HFSM2)
      add(spec, LibraryKind::Hfsm2, &backend_hfsm2::run_hierarchical);
#endif
#if defined(HSM_BENCH_ENABLE_QP)
      add(spec, LibraryKind::Qp, &backend_qp::run_hierarchical);
#endif
    } else if (std::strcmp(spec.id, "deep") == 0) {
      add(spec, LibraryKind::Hsm, &backend_hsm::run_deep);
      add(spec, LibraryKind::VanillaSwitch, &backend_vanilla_switch::run_deep);
      add(spec, LibraryKind::VanillaFp, &backend_vanilla_fp::run_deep);
#if defined(HSM_BENCH_ENABLE_SML)
      add(spec, LibraryKind::Sml, &backend_sml::run_deep);
#endif
#if defined(HSM_BENCH_ENABLE_HSMCPP)
      add(spec, LibraryKind::Hsmcpp, &backend_hsmcpp::run_deep);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
      add(spec, LibraryKind::BoostMsm, &backend_boost_msm::run_deep);
#endif
#if defined(HSM_BENCH_ENABLE_HFSM2)
      add(spec, LibraryKind::Hfsm2, &backend_hfsm2::run_deep);
#endif
#if defined(HSM_BENCH_ENABLE_QP)
      add(spec, LibraryKind::Qp, &backend_qp::run_deep);
#endif
    } else if (std::strcmp(spec.id, "guarded") == 0) {
      add(spec, LibraryKind::Hsm, &backend_hsm::run_guarded);
      add(spec, LibraryKind::VanillaSwitch,
          &backend_vanilla_switch::run_guarded);
      add(spec, LibraryKind::VanillaFp, &backend_vanilla_fp::run_guarded);
#if defined(HSM_BENCH_ENABLE_SML)
      // SML guarded scenario often optimized away, but we register it
      add(spec, LibraryKind::Sml, &backend_sml::run_guarded);
#endif
#if defined(HSM_BENCH_ENABLE_HSMCPP)
      add(spec, LibraryKind::Hsmcpp, &backend_hsmcpp::run_guarded);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
      add(spec, LibraryKind::BoostMsm, &backend_boost_msm::run_guarded);
#endif
#if defined(HSM_BENCH_ENABLE_HFSM2)
      add(spec, LibraryKind::Hfsm2, &backend_hfsm2::run_guarded);
#endif
#if defined(HSM_BENCH_ENABLE_QP)
      add(spec, LibraryKind::Qp, &backend_qp::run_guarded);
#endif
    } else if (std::strcmp(spec.id, "traffic_light") == 0) {
      add(spec, LibraryKind::Hsm, &backend_hsm::run_traffic_light);
      add(spec, LibraryKind::HsmThreaded, &backend_hsm::run_traffic_light_threaded);
      add(spec, LibraryKind::VanillaSwitch,
          &backend_vanilla_switch::run_traffic_light);
      add(spec, LibraryKind::VanillaFp,
          &backend_vanilla_fp::run_traffic_light);
#if defined(HSM_BENCH_ENABLE_SML)
      add(spec, LibraryKind::Sml, &backend_sml::run_traffic_light);
#endif
#if defined(HSM_BENCH_ENABLE_HSMCPP)
      add(spec, LibraryKind::Hsmcpp, &backend_hsmcpp::run_traffic_light);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
      add(spec, LibraryKind::BoostMsm, &backend_boost_msm::run_traffic_light);
#endif
#if defined(HSM_BENCH_ENABLE_HFSM2)
      add(spec, LibraryKind::Hfsm2, &backend_hfsm2::run_traffic_light);
#endif
#if defined(HSM_BENCH_ENABLE_QP)
      add(spec, LibraryKind::Qp, &backend_qp::run_traffic_light);
#endif
    }
  }
  return runners;
}

// -----------------------------------------------------------------------------
// Reporting helpers
// -----------------------------------------------------------------------------

static void compute_relative_per_scenario(std::vector<BenchmarkRow> &rows,
                                          const std::string &baseline_name) {
  // For each scenario, find the baseline transitions/sec.
  std::unordered_map<std::string, double> baseline;
  for (const auto &row : rows) {
    if (row.library == baseline_name) {
      auto it = baseline.find(row.scenario);
      if (it == baseline.end()) {
        baseline.emplace(row.scenario, row.transitions_per_second);
      }
    }
  }

  for (auto &row : rows) {
    auto it = baseline.find(row.scenario);
    if (it == baseline.end() || it->second <= 0.0) {
      row.percent_vs_baseline = 0.0;
    } else {
      const double base = it->second;
      row.percent_vs_baseline =
          ((row.transitions_per_second - base) / base) * 100.0;
    }
  }
}

static void print_text_results(const std::vector<BenchmarkRow> &rows) {
  if (rows.empty()) {
    std::cout << "No benchmark results to display.\n";
    return;
  }

  std::cout << "HSM Comparison Benchmark" << std::endl;
  std::cout << "========================" << std::endl;

  // Group by scenario for nicer output.
  std::unordered_map<std::string, std::vector<const BenchmarkRow *>> by_scenario;
  for (const auto &row : rows) {
    by_scenario[row.scenario].push_back(&row);
  }

  // Stable order for scenarios: iterate through known scenarios (kScenarios)
  // and print if present in the results.
  for (const auto &spec : kScenarios) {
    const auto &scenario_name = spec.name;
    auto it = by_scenario.find(scenario_name);
    if (it == by_scenario.end()) continue;

    const auto &vec = it->second;

    std::cout << "\nScenario: " << scenario_name << "\n";
    std::cout << std::left << std::setw(18) << "Library" << std::right
              << std::setw(16) << "Disp/sec" << std::setw(16)
              << "Trans/sec" << std::setw(12) << "% vs base" << std::endl;

    for (const BenchmarkRow *r : vec) {
      std::cout << std::left << std::setw(18) << r->library << std::right
                << std::setw(16) << std::fixed << std::setprecision(0)
                << r->dispatches_per_second << std::setw(16)
                << std::fixed << std::setprecision(0)
                << r->transitions_per_second << std::setw(12)
                << std::fixed << std::setprecision(1) << std::showpos
                << r->percent_vs_baseline << std::noshowpos << std::endl;
    }
  }
}

static std::string make_output_path(const std::string &prefix,
                                    const std::string &filename) {
  if (prefix.empty()) return filename;
  if (!prefix.empty() &&
      (prefix.back() == '/' || prefix.back() == '\\')) {
    return prefix + filename;
  }
  return prefix + "/" + filename;
}

static bool ensure_parent_directory(const std::string &path) {
  std::error_code ec;
  std::filesystem::path p(path);
  auto dir = p.parent_path();
  if (dir.empty()) {
    return true;
  }
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    std::cerr << "Failed to create directory '" << dir.string() << "': "
              << ec.message() << "\n";
    return false;
  }
  return true;
}

static std::string make_run_id() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto t = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm) == 0) {
    return "unknown";
  }
  return std::string(buf);
}

static void write_csv(const std::vector<BenchmarkRow> &rows,
                      const GlobalConfig &cfg) {
  const std::string filename = "hsm_bench_results_" + make_run_id() + ".csv";
  const std::string path = make_output_path(cfg.output_prefix, filename);

  if (!ensure_parent_directory(path)) {
    return;
  }

  std::ofstream csv(path);
  if (!csv.is_open()) {
    std::cerr << "Failed to open CSV file: " << path << "\n";
    return;
  }

  csv << "Scenario,Library,Disp/sec,Trans/sec,PercentVsBaseline,Iterations,"
         "DispPerIter,TransPerDisp\n";
  for (const auto &r : rows) {
    csv << '"' << r.scenario << '"' << ',' << '"' << r.library << '"' << ','
        << std::fixed << std::setprecision(0) << r.dispatches_per_second << ','
        << std::fixed << std::setprecision(0) << r.transitions_per_second
        << ',' << std::fixed << std::setprecision(1)
        << r.percent_vs_baseline << ',' << r.iterations << ','
        << r.dispatches_per_iteration << ','
        << std::fixed << std::setprecision(3)
        << r.transitions_per_dispatch << "\n";
  }

  std::cout << "CSV snapshot written to " << path << "\n";
}

static void write_json(const std::vector<BenchmarkRow> &rows,
                       const GlobalConfig &cfg) {
  const std::string filename = "hsm_bench_results_" + make_run_id() + ".json";
  const std::string path = make_output_path(cfg.output_prefix, filename);

  if (!ensure_parent_directory(path)) {
    return;
  }

  std::ofstream json(path);
  if (!json.is_open()) {
    std::cerr << "Failed to open JSON file: " << path << "\n";
    return;
  }

  json << "[\n";
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto &r = rows[i];
    json << "  {\n";
    json << "    \"scenario\": \"" << r.scenario << "\",\n";
    json << "    \"library\": \"" << r.library << "\",\n";
    json << "    \"dispatches_per_second\": " << std::fixed
         << std::setprecision(0) << r.dispatches_per_second << ",\n";
    json << "    \"transitions_per_second\": " << std::fixed
         << std::setprecision(0) << r.transitions_per_second << ",\n";
    json << "    \"percent_vs_baseline\": " << std::fixed
         << std::setprecision(1) << r.percent_vs_baseline << ",\n";
    json << "    \"iterations\": " << r.iterations << ",\n";
    json << "    \"dispatches_per_iteration\": " << r.dispatches_per_iteration
         << ",\n";
    json << "    \"transitions_per_dispatch\": " << std::fixed
         << std::setprecision(3) << r.transitions_per_dispatch << "\n";
    json << "  }" << (i < rows.size() - 1 ? "," : "") << "\n";
  }
  json << "]\n";

  std::cout << "JSON snapshot written to " << path << "\n";
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char **argv) {
  auto cfg_opt = parse_args(argc, argv);
  if (!cfg_opt) return 1;
  const GlobalConfig &cfg = *cfg_opt;

  auto runners = build_runners(cfg);
  if (runners.empty()) {
    std::cerr << "No benchmark scenarios selected or available.\n";
    return 1;
  }

  std::cout << "Running " << runners.size() << " benchmark scenarios...\n";
  std::cout << "Configuration:\n";
  std::cout << "  Warmup:       " << cfg.warmup_iterations << "\n";
  std::cout << "  Iterations:   " << cfg.iterations << "\n";
  std::cout << "  Baseline:     " << cfg.baseline << "\n";
  std::cout << "  Output dir:   " << (cfg.output_prefix.empty() ? "." : cfg.output_prefix) << "\n";
  if (!cfg.scenario_ids.empty()) {
    std::cout << "  Scenarios:    " << cfg.scenario_ids << "\n";
  } else if (!cfg.scenario_filter.empty()) {
    std::cout << "  Name filter:  " << cfg.scenario_filter << "\n";
  }
  std::cout << "\n";

  std::vector<BenchmarkRow> results;
  results.reserve(runners.size());

  for (const auto &runner : runners) {
    // std::cout << "Running " << runner.library << " / " << runner.scenario->name << "..." << std::flush;
    auto row = runner.run(cfg, *runner.scenario);
    results.push_back(row);
    // std::cout << " Done.\n";
  }

  compute_relative_per_scenario(results, cfg.baseline);

  if (cfg.format == GlobalConfig::Format::Text ||
      cfg.format == GlobalConfig::Format::All) {
    print_text_results(results);
  }
  if (cfg.format == GlobalConfig::Format::Csv ||
      cfg.format == GlobalConfig::Format::All) {
    write_csv(results, cfg);
  }
  if (cfg.format == GlobalConfig::Format::Json ||
      cfg.format == GlobalConfig::Format::All) {
    write_json(results, cfg);
  }

  return 0;
}
