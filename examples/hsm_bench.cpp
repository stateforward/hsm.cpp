#include <chrono>
#include <cstring>
#include <functional>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>

#include "hsm/hsm.hpp"
#include "hsm/kind.hpp"

// Simple, self-contained benchmark utility for hsm::compile
// Focus: runtime throughput of different model shapes and feature combinations.

using Clock = std::chrono::steady_clock;

struct GlobalConfig {
  std::size_t warmup_iterations = 1000;
  std::size_t iterations = 10000;

  enum class Format { Text, Csv, Json, All } format = Format::All;

  std::string filter;         // substring filter on scenario name
  std::string output_prefix;  // optional prefix for CSV/JSON filenames
};

struct BenchmarkResult {
  std::string name;
  std::size_t iterations{};
  std::size_t operations_per_iteration{};
  double operations_per_second{};
  double percent_change{};  // relative to first result in run
};

// ------------------------- CLI parsing helpers -------------------------

static void print_usage(const char *argv0) {
  std::cout << "Usage: " << argv0
            << " [--warmup=N] [--iterations=N] [--filter=substr]"
               " [--format=text|csv|json|all] [--output-prefix=PATH]\n";
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
      cfg.filter = val;
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

// ---------------------------- Harness core -----------------------------

template <typename Loop>
static double measure_throughput(std::size_t warmup_iterations,
                                 std::size_t measured_iterations,
                                 std::size_t operations_per_iteration,
                                 Loop &&loop) {
  using namespace std::chrono;

  for (std::size_t i = 0; i < warmup_iterations; ++i) {
    loop();
  }

  const auto start = Clock::now();
  for (std::size_t i = 0; i < measured_iterations; ++i) {
    loop();
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

// ------------------------ Core dispatch scenarios ----------------------

using namespace hsm;

struct BenchmarkInstance {
  // Extendable for scenarios that need counters/state.
};

// Events reused across several scenarios
struct ToChild1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct ToChild2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct ToLevel3a : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct ToLevel3b : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct ToParent1 : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct ToParent2 : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};

// Simple no-op behaviors used in some variants
static void no_behavior(Signal &, BenchmarkInstance &, const EventBase &) {}

// 1. Nested states (no actions)
static constexpr auto model_nested =
    define("BenchNested",
           state("parent", state("child1"), state("child2"),
                 initial(target("/BenchNested/parent/child1")),
                 transition(on<ToChild2>(), source("/BenchNested/parent/child1"),
                            target("/BenchNested/parent/child2")),
                 transition(on<ToChild1>(), source("/BenchNested/parent/child2"),
                            target("/BenchNested/parent/child1"))),
           initial(target("/BenchNested/parent")));

// 1.a With entry
static constexpr auto model_nested_entry = define(
    "BenchNestedEntry",
    state("parent", entry(no_behavior), state("child1", entry(no_behavior)),
          state("child2", entry(no_behavior)),
          initial(target("/BenchNestedEntry/parent/child1")),
          transition(on<ToChild2>(), source("/BenchNestedEntry/parent/child1"),
                     target("/BenchNestedEntry/parent/child2")),
          transition(on<ToChild1>(), source("/BenchNestedEntry/parent/child2"),
                     target("/BenchNestedEntry/parent/child1"))),
    initial(target("/BenchNestedEntry/parent")));

// 1.b Entry + Activity
static void yield_activity(Signal &, BenchmarkInstance &, const EventBase &) {
  // We deliberately avoid std::this_thread::sleep_for here to keep benchmarks
  // deterministic and fast; this is a no-op activity.
}

static constexpr auto model_nested_entry_activity =
    define("BenchNestedEntryActivity",
           state("parent", entry(no_behavior), activity(yield_activity),
                 state("child1", entry(no_behavior), activity(yield_activity)),
                 state("child2", entry(no_behavior), activity(yield_activity)),
                 initial(target("/BenchNestedEntryActivity/parent/child1")),
                 transition(on<ToChild2>(),
                            source("/BenchNestedEntryActivity/parent/child1"),
                            target("/BenchNestedEntryActivity/parent/child2")),
                 transition(on<ToChild1>(),
                            source("/BenchNestedEntryActivity/parent/child2"),
                            target("/BenchNestedEntryActivity/parent/child1"))),
           initial(target("/BenchNestedEntryActivity/parent")));

// 1.c Entry + Exit + Activity
static constexpr auto model_nested_entry_exit_activity =
    define("BenchNestedEntryExitActivity",
           state("parent", entry(no_behavior), exit(no_behavior),
                 activity(yield_activity),
                 state("child1", entry(no_behavior), exit(no_behavior),
                       activity(yield_activity)),
                 state("child2", entry(no_behavior), exit(no_behavior),
                       activity(yield_activity)),
                 initial(target("/BenchNestedEntryExitActivity/parent/child1")),
                 transition(on<ToChild2>(),
                            source("/BenchNestedEntryExitActivity/parent/child1"),
                            target("/BenchNestedEntryExitActivity/parent/child2")),
                 transition(on<ToChild1>(),
                            source("/BenchNestedEntryExitActivity/parent/child2"),
                            target("/BenchNestedEntryExitActivity/parent/child1"))),
           initial(target("/BenchNestedEntryExitActivity/parent")));

// 1.d Entry + Exit + Activity + Effect
static constexpr auto model_nested_full = define(
    "BenchNestedFull",
    state("parent", entry(no_behavior), exit(no_behavior),
          activity(yield_activity),
          state("child1", entry(no_behavior), exit(no_behavior),
                activity(yield_activity)),
          state("child2", entry(no_behavior), exit(no_behavior),
                activity(yield_activity)),
          initial(target("/BenchNestedFull/parent/child1")),
          transition(on<ToChild2>(), source("/BenchNestedFull/parent/child1"),
                     target("/BenchNestedFull/parent/child2"),
                     effect(no_behavior)),
          transition(on<ToChild1>(), source("/BenchNestedFull/parent/child2"),
                     target("/BenchNestedFull/parent/child1"),
                     effect(no_behavior))),
    initial(target("/BenchNestedFull/parent")));

// Deep nesting model
static constexpr auto model_deep = define(
    "BenchDeep",
    state("level1", entry(no_behavior), exit(no_behavior),
          state("level2", entry(no_behavior), exit(no_behavior),
                state("level3a", entry(no_behavior), exit(no_behavior)),
                state("level3b", entry(no_behavior), exit(no_behavior)),
                initial(target("/BenchDeep/level1/level2/level3a")),
                transition(on<ToLevel3b>(),
                           source("/BenchDeep/level1/level2/level3a"),
                           target("/BenchDeep/level1/level2/level3b")),
                transition(on<ToLevel3a>(),
                           source("/BenchDeep/level1/level2/level3b"),
                           target("/BenchDeep/level1/level2/level3a"))),
          initial(target("/BenchDeep/level1/level2"))),
    initial(target("/BenchDeep/level1")));

// Cross hierarchy model
static constexpr auto model_cross =
    define("BenchCross",
           state("parent1", entry(no_behavior), exit(no_behavior),
                 state("child1", entry(no_behavior), exit(no_behavior)),
                 initial(target("/BenchCross/parent1/child1"))),
           state("parent2", entry(no_behavior), exit(no_behavior),
                 state("child2", entry(no_behavior), exit(no_behavior)),
                 initial(target("/BenchCross/parent2/child2"))),
           transition(on<ToParent2>(), source("/BenchCross/parent1"),
                      target("/BenchCross/parent2")),
           transition(on<ToParent1>(), source("/BenchCross/parent2"),
                      target("/BenchCross/parent1")),
           initial(target("/BenchCross/parent1")));

// CRTP machines for each benchmark model
struct NestedMachine : BenchmarkInstance, HSM<model_nested, NestedMachine> {};
struct NestedEntryMachine : BenchmarkInstance, HSM<model_nested_entry, NestedEntryMachine> {};
struct NestedEntryActivityMachine : BenchmarkInstance, HSM<model_nested_entry_activity, NestedEntryActivityMachine> {};
struct NestedEntryExitActivityMachine : BenchmarkInstance, HSM<model_nested_entry_exit_activity, NestedEntryExitActivityMachine> {};
struct NestedFullMachine : BenchmarkInstance, HSM<model_nested_full, NestedFullMachine> {};
struct DeepMachine : BenchmarkInstance, HSM<model_deep, DeepMachine> {};
struct CrossMachine : BenchmarkInstance, HSM<model_cross, CrossMachine> {};

// Scenario helpers

static BenchmarkResult run_nested_baseline(const GlobalConfig &cfg) {
  NestedMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToChild2>();
    sm.template dispatch<ToChild1>();
  };

  BenchmarkResult r;
  r.name = "1. Nested states (no entry/exit/activity)";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;  // two dispatches per iteration
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_nested_entry(const GlobalConfig &cfg) {
  NestedEntryMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToChild2>();
    sm.template dispatch<ToChild1>();
  };

  BenchmarkResult r;
  r.name = "1.a With entry";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_nested_entry_activity(const GlobalConfig &cfg) {
  NestedEntryActivityMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToChild2>();
    sm.template dispatch<ToChild1>();
  };

  BenchmarkResult r;
  r.name = "1.b With entry+activity (no-op)";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_nested_entry_exit_activity(const GlobalConfig &cfg) {
  NestedEntryExitActivityMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToChild2>();
    sm.template dispatch<ToChild1>();
  };

  BenchmarkResult r;
  r.name = "1.c With entry+exit+activity (no-op)";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_nested_full(const GlobalConfig &cfg) {
  NestedFullMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToChild2>();
    sm.template dispatch<ToChild1>();
  };

  BenchmarkResult r;
  r.name = "1.d With entry+exit+activity+effect (no-op)";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_deep(const GlobalConfig &cfg) {
  DeepMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToLevel3b>();
    sm.template dispatch<ToLevel3a>();
  };

  BenchmarkResult r;
  r.name = "Deep nesting";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_cross(const GlobalConfig &cfg) {
  CrossMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<ToParent2>();
    sm.template dispatch<ToParent1>();
  };

  BenchmarkResult r;
  r.name = "Cross hierarchy";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 2;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

namespace typed_bench {

struct StartEvent
    : hsm::Event<hsm::make_kind(100, hsm::Kind::Event)> {};
struct PayloadEvent
    : hsm::Event<hsm::make_kind(101, hsm::Kind::Event)> {
  int value{};
};
// Note: a previous version included an UnrelatedEvent used to
// benchmark mismatched event dispatch. In the new design, dispatching
// an event type that the model does not support is a compile-time
// error, so that scenario has been removed.
struct Device {
  int runtime_entries = 0;
  int typed_entries = 0;
  int guard_checks = 0;
  int effect_calls = 0;
  int payload_sum = 0;
};

static constexpr auto typed_model = define(
    "device", initial(target("/device/idle")),
    state("idle",
          entry([](hsm::Signal &, Device &d, const hsm::EventBase &) {
            ++d.runtime_entries;
          }),
          entry([](hsm::Signal &, Device &d, const StartEvent &) {
            ++d.typed_entries;
          }),
          transition(
              on<StartEvent>(), guard([](Device &d, const StartEvent &) {
                ++d.guard_checks;
                return true;
              }),
              effect([](Device &d, const StartEvent &) { ++d.effect_calls; }),
              target("/device/idle"))),
    state("active"));

static constexpr auto payload_model = define(
    "payload_device", initial(target("/payload_device/idle")),
    state("idle", transition(on<PayloadEvent>(),
                             effect([](Device &d, const PayloadEvent &evt) {
                               d.payload_sum += evt.value;
                             }),
                             target("/payload_device/idle"))));

// CRTP machines for typed-event benchmarks
struct TypedMachine : Device, HSM<typed_model, TypedMachine> {};
struct PayloadMachine : Device, HSM<payload_model, PayloadMachine> {};

}  // namespace typed_bench

static BenchmarkResult run_typed_event_happy(const GlobalConfig &cfg) {
  typed_bench::TypedMachine sm;

  auto loop = [&]() noexcept {
    sm.template dispatch<typed_bench::StartEvent>();
  };

  BenchmarkResult r;
  r.name = "Typed events - matching StartEvent";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 1;  // one dispatch per iteration
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

static BenchmarkResult run_typed_payload(const GlobalConfig &cfg) {
  typed_bench::PayloadMachine sm;

  auto loop = [&]() noexcept {
    typed_bench::PayloadEvent p{};
    p.value = 1;
    sm.dispatch(p);
  };

  BenchmarkResult r;
  r.name = "Typed events - payload";
  r.iterations = cfg.iterations;
  r.operations_per_iteration = 1;
  r.operations_per_second =
      measure_throughput(cfg.warmup_iterations, cfg.iterations,
                         r.operations_per_iteration, loop);
  return r;
}

// --------------------------- Scenario registry -------------------------

struct Scenario {
  std::string name;
  std::function<BenchmarkResult(const GlobalConfig &)> run;
};

static std::vector<Scenario> build_scenarios() {
  std::vector<Scenario> scenarios;

  // Core dispatch / hierarchy ladder
  scenarios.push_back({"1. Nested states (no entry/exit/activity)",
                       &run_nested_baseline});
  scenarios.push_back({"1.a With entry", &run_nested_entry});
  scenarios.push_back({"1.b With entry+activity (no-op)",
                       &run_nested_entry_activity});
  scenarios.push_back({"1.c With entry+exit+activity (no-op)",
                       &run_nested_entry_exit_activity});
  scenarios.push_back({"1.d With entry+exit+activity+effect (no-op)",
                       &run_nested_full});
  scenarios.push_back({"Deep nesting", &run_deep});
  scenarios.push_back({"Cross hierarchy", &run_cross});

  // Typed-event scenarios
  scenarios.push_back({"Typed events - matching StartEvent",
                       &run_typed_event_happy});
  scenarios.push_back({"Typed events - payload", &run_typed_payload});

  return scenarios;
}

// --------------------------- Reporting output --------------------------

static void compute_relative_changes(std::vector<BenchmarkResult> &results) {
  if (results.empty()) return;
  const double baseline = results.front().operations_per_second;
  if (baseline <= 0.0) {
    for (auto &r : results) r.percent_change = 0.0;
    return;
  }
  results.front().percent_change = 0.0;
  for (std::size_t i = 1; i < results.size(); ++i) {
    auto &r = results[i];
    r.percent_change =
        ((r.operations_per_second - baseline) / baseline) * 100.0;
  }
}

static void print_text_results(const std::vector<BenchmarkResult> &results) {
  std::cout << "HSM Benchmark" << std::endl;
  std::cout << "================" << std::endl;

  std::cout << std::left << std::setw(60) << "Scenario" << std::right
            << std::setw(16) << "Ops/sec" << std::setw(12) << "% change"
            << std::endl;

  for (const auto &r : results) {
    std::cout << std::left << std::setw(60) << r.name << std::right
              << std::setw(16) << std::fixed << std::setprecision(0)
              << r.operations_per_second << std::setw(12) << std::setprecision(1)
              << std::showpos << r.percent_change << std::noshowpos
              << std::endl;
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

static void write_csv(const std::vector<BenchmarkResult> &results,
                      const GlobalConfig &cfg) {
  const std::string path =
      make_output_path(cfg.output_prefix, "hsm_benchmark_results.csv");
  std::ofstream csv(path);
  if (!csv.is_open()) {
    std::cerr << "Failed to open CSV file: " << path << "\n";
    return;
  }
  csv << "Scenario,Ops/sec,Change %,Iterations,Ops/iteration\n";
  for (const auto &r : results) {
    csv << '"' << r.name << '"' << ',' << std::fixed
        << std::setprecision(0) << r.operations_per_second << ','
        << std::setprecision(1) << r.percent_change << ',' << r.iterations
        << ',' << r.operations_per_iteration << "\n";
  }
  std::cout << "CSV results written to " << path << "\n";
}

static void write_json(const std::vector<BenchmarkResult> &results,
                       const GlobalConfig &cfg) {
  const std::string path =
      make_output_path(cfg.output_prefix, "hsm_benchmark_results.json");
  std::ofstream json(path);
  if (!json.is_open()) {
    std::cerr << "Failed to open JSON file: " << path << "\n";
    return;
  }

  json << "{\n";
  json << "  \"results\": [\n";
  for (std::size_t i = 0; i < results.size(); ++i) {
    const auto &r = results[i];
    json << "    {\n";
    json << "      \"name\": \"" << r.name << "\",\n";
    json << "      \"iterations\": " << r.iterations << ",\n";
    json << "      \"operationsPerIteration\": "
         << r.operations_per_iteration << ",\n";
    json << "      \"operationsPerSecond\": " << r.operations_per_second
         << ",\n";
    json << "      \"percentChange\": " << r.percent_change << "\n";
    json << "    }";
    if (i + 1 < results.size()) json << ',';
    json << "\n";
  }
  json << "  ]\n";
  json << "}\n";

  std::cout << "JSON results written to " << path << "\n";
}

// -------------------------------- main ---------------------------------

int main(int argc, char **argv) {
  auto maybe_cfg = parse_args(argc, argv);
  if (!maybe_cfg.has_value()) {
    return 1;  // help or error already printed
  }
  GlobalConfig cfg = *maybe_cfg;

  auto scenarios = build_scenarios();
  std::vector<BenchmarkResult> results;
  results.reserve(scenarios.size());

  for (const auto &s : scenarios) {
    if (!cfg.filter.empty() && s.name.find(cfg.filter) == std::string::npos) {
      continue;
    }
    results.push_back(s.run(cfg));
  }

  if (results.empty()) {
    std::cerr << "No scenarios matched filter; nothing to run.\n";
    return 1;
  }

  compute_relative_changes(results);

  switch (cfg.format) {
    case GlobalConfig::Format::Text:
      print_text_results(results);
      break;
    case GlobalConfig::Format::Csv:
      write_csv(results, cfg);
      break;
    case GlobalConfig::Format::Json:
      write_json(results, cfg);
      break;
    case GlobalConfig::Format::All:
      print_text_results(results);
      write_csv(results, cfg);
      write_json(results, cfg);
      break;
  }

  return 0;
}
