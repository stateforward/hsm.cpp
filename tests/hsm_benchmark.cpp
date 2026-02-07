#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hsm/hsm.hpp"
#include "hsm/kind.hpp"

// Struct to hold benchmark results
struct BenchmarkResult {
  std::string name;
  double transitionsPerSecond;
  double percentChange;
  size_t memoryUsedBytes;
  size_t peakMemoryBytes;  // Not easily measurable portably without OS headers
  int iterations;
};

// Dummy memory usage (hard to measure portably/consistently in this script
// without headers)
size_t getCurrentMemoryUsage() {
  return 0;  // Placeholder
}

using namespace hsm;

using BenchEventBase = hsm::AnyEvent;

// using Scheduler = hsm::Scheduler<>; // Remove this ambiguous using
// Removed BenchScheduler template
// BenchScheduler no longer needed

struct BenchmarkInstance {
  // Data for activities or actions if needed
};

// Behaviors
void noBehavior(Signal&, BenchmarkInstance&, const BenchEventBase&) {}

void activityBehavior(Signal&, BenchmarkInstance&, const BenchEventBase&) {
  std::this_thread::yield();
}

// Define events
struct ToChild1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct ToChild2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct ToLevel3a : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct ToLevel3b : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};
struct ToParent1 : hsm::Event<hsm::make_kind(5, hsm::Kind::Event)> {};
struct ToParent2 : hsm::Event<hsm::make_kind(6, hsm::Kind::Event)> {};
struct ValidEvent : hsm::Event<hsm::make_kind(7, hsm::Kind::Event)> {};

// Generic benchmark runner
template <typename SM, typename InstanceType, typename Event1, typename Event2>
BenchmarkResult runBenchmark(const std::string& scenarioName,
                             int warmupIterations = 1000,
                             int benchmarkIterations = 10000,
                             double* baselineSpeed = nullptr) {
  BenchmarkResult result;
  result.name = scenarioName;
  result.iterations = benchmarkIterations;

  // Create instance and start
  SM sm;

  // Warmup
  for (int i = 0; i < warmupIterations; i++) {
    sm.template dispatch<Event1>();
    sm.template dispatch<Event2>();
  }

  // Benchmark
  auto start = std::chrono::high_resolution_clock::now();

  for (int i = 0; i < benchmarkIterations; i++) {
    sm.template dispatch<Event1>();
    sm.template dispatch<Event2>();
  }

  auto end = std::chrono::high_resolution_clock::now();

  // Calculate transitions per second
  auto duration =
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count();
  double totalTransitions = static_cast<double>(benchmarkIterations) *
                            2.0;  // Two transitions per iteration
  result.transitionsPerSecond =
      (totalTransitions / static_cast<double>(duration)) * 1000000.0;

  // Calculate percentage change if baseline provided
  result.percentChange = 0.0;
  if (baselineSpeed != nullptr) {
    if (*baselineSpeed < 1e-9) {  // Use epsilon comparison instead of ==
      *baselineSpeed = result.transitionsPerSecond;
    } else {
      result.percentChange =
          ((result.transitionsPerSecond - *baselineSpeed) / *baselineSpeed) *
          100.0;
    }
  }

  // Print to console
  std::cout << std::left << std::setw(65) << scenarioName << std::right
            << std::setw(12) << std::fixed << std::setprecision(0)
            << result.transitionsPerSecond << " trans/sec";

  if (baselineSpeed != nullptr &&
      std::abs(*baselineSpeed - result.transitionsPerSecond) > 1e-9) {  // Use epsilon comparison
    std::cout << " (" << std::showpos << std::setprecision(1)
              << result.percentChange << "%" << std::noshowpos << ")";
  } else if (baselineSpeed != nullptr) {
    std::cout << " (baseline)";
  }
  std::cout << std::endl;

  return result;
}

void writeResultsToCSV(const std::vector<BenchmarkResult>& results,
                       const std::string& filename) {
  std::ofstream csv(filename);
  csv << "Scenario,Transitions/sec,Change %,Iterations\n";
  for (const auto& result : results) {
    csv << "\"" << result.name << "\"," << std::fixed << std::setprecision(0)
        << result.transitionsPerSecond << "," << std::fixed
        << std::setprecision(1) << result.percentChange << ","
        << result.iterations << "\n";
  }
  csv.close();
  std::cout << "\nResults written to " << filename << std::endl;
}

void writeResultsToJSON(const std::vector<BenchmarkResult>& results,
                        const std::string& filename) {
  std::ofstream json(filename);
  json << "{\n";
  json << "  \"timestamp\": \""
       << std::chrono::system_clock::now().time_since_epoch().count()
       << "\",\n";
  json << "  \"results\": [\n";
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& result = results[i];
    json << "    {\n";
    json << "      \"name\": \"" << result.name << "\",\n";
    json << "      \"transitionsPerSecond\": " << result.transitionsPerSecond
         << ",\n";
    json << "      \"percentChange\": " << result.percentChange << ",\n";
    json << "      \"iterations\": " << result.iterations << "\n";
    json << "    }";
    if (i < results.size() - 1) json << ",";
    json << "\n";
  }
  json << "  ]\n";
  json << "}\n";
  json.close();
  std::cout << "Results written to " << filename << std::endl;
}

// Define models as constexpr
// 1. Nested states (no actions)
constexpr auto model1 =
    define("TestHSM1",
           state("parent", state("child1"), state("child2"),
                 initial(target("/TestHSM1/parent/child1")),
                 transition(on<ToChild2>(), source("/TestHSM1/parent/child1"),
                            target("/TestHSM1/parent/child2")),
                 transition(on<ToChild1>(), source("/TestHSM1/parent/child2"),
                            target("/TestHSM1/parent/child1"))),
           initial(target("/TestHSM1/parent")));

// 1.a With entry
constexpr auto model1a = define(
    "TestHSM1a",
    state("parent", entry(noBehavior), state("child1", entry(noBehavior)),
          state("child2", entry(noBehavior)),
          initial(target("/TestHSM1a/parent/child1")),
          transition(on<ToChild2>(), source("/TestHSM1a/parent/child1"),
                     target("/TestHSM1a/parent/child2")),
          transition(on<ToChild1>(), source("/TestHSM1a/parent/child2"),
                     target("/TestHSM1a/parent/child1"))),
    initial(target("/TestHSM1a/parent")));

// 1.b Entry + Activity
constexpr auto model1b =
    define("TestHSM1b",
           state("parent", entry(noBehavior), activity(activityBehavior),
                 state("child1", entry(noBehavior), activity(activityBehavior)),
                 state("child2", entry(noBehavior), activity(activityBehavior)),
                 initial(target("/TestHSM1b/parent/child1")),
                 transition(on<ToChild2>(), source("/TestHSM1b/parent/child1"),
                            target("/TestHSM1b/parent/child2")),
                 transition(on<ToChild1>(), source("/TestHSM1b/parent/child2"),
                            target("/TestHSM1b/parent/child1"))),
           initial(target("/TestHSM1b/parent")));

// 1.c Entry + Exit + Activity
constexpr auto model1c =
    define("TestHSM1c",
           state("parent", entry(noBehavior), exit(noBehavior),
                 activity(activityBehavior),
                 state("child1", entry(noBehavior), exit(noBehavior),
                       activity(activityBehavior)),
                 state("child2", entry(noBehavior), exit(noBehavior),
                       activity(activityBehavior)),
                 initial(target("/TestHSM1c/parent/child1")),
                 transition(on<ToChild2>(), source("/TestHSM1c/parent/child1"),
                            target("/TestHSM1c/parent/child2")),
                 transition(on<ToChild1>(), source("/TestHSM1c/parent/child2"),
                            target("/TestHSM1c/parent/child1"))),
           initial(target("/TestHSM1c/parent")));

// 1.d Entry + Exit + Activity + Effect
constexpr auto model1d = define(
    "TestHSM1d",
    state("parent", entry(noBehavior), exit(noBehavior),
          activity(activityBehavior),
          state("child1", entry(noBehavior), exit(noBehavior),
                activity(activityBehavior)),
          state("child2", entry(noBehavior), exit(noBehavior),
                activity(activityBehavior)),
          initial(target("/TestHSM1d/parent/child1")),
          transition(on<ToChild2>(), source("/TestHSM1d/parent/child1"),
                     target("/TestHSM1d/parent/child2"), effect(noBehavior)),
          transition(on<ToChild1>(), source("/TestHSM1d/parent/child2"),
                     target("/TestHSM1d/parent/child1"), effect(noBehavior))),
    initial(target("/TestHSM1d/parent")));

// Deep Nesting
constexpr auto modelDeep = define(
    "TestHSMDeep",
    state("level1", entry(noBehavior), exit(noBehavior),
          state("level2", entry(noBehavior), exit(noBehavior),
                state("level3a", entry(noBehavior), exit(noBehavior)),
                state("level3b", entry(noBehavior), exit(noBehavior)),
                initial(target("/TestHSMDeep/level1/level2/level3a")),
                transition(on<ToLevel3b>(),
                           source("/TestHSMDeep/level1/level2/level3a"),
                           target("/TestHSMDeep/level1/level2/level3b")),
                transition(on<ToLevel3a>(),
                           source("/TestHSMDeep/level1/level2/level3b"),
                           target("/TestHSMDeep/level1/level2/level3a"))),
          initial(target("/TestHSMDeep/level1/level2"))),
    initial(target("/TestHSMDeep/level1")));

// Cross Hierarchy
constexpr auto modelCross =
    define("TestHSMCrossHierarchy",
           state("parent1", entry(noBehavior), exit(noBehavior),
                 state("child1", entry(noBehavior), exit(noBehavior)),
                 initial(target("/TestHSMCrossHierarchy/parent1/child1"))),
           state("parent2", entry(noBehavior), exit(noBehavior),
                 state("child2", entry(noBehavior), exit(noBehavior)),
                 initial(target("/TestHSMCrossHierarchy/parent2/child2"))),
           transition(on<ToParent2>(), source("/TestHSMCrossHierarchy/parent1"),
                      target("/TestHSMCrossHierarchy/parent2")),
           transition(on<ToParent1>(), source("/TestHSMCrossHierarchy/parent2"),
                      target("/TestHSMCrossHierarchy/parent1")),
           initial(target("/TestHSMCrossHierarchy/parent1")));

// NOTE: A previous version of this benchmark included a scenario that
// repeatedly dispatched event types that were not present in the model in
// order to measure the cost of failed lookups. The HSM now treats using
// event types that are not part of the model as a compile-time error, so
// that scenario has been removed.

int main() {
  std::cout << "HSM Benchmark" << std::endl;
  std::cout << "=================" << std::endl;

  std::vector<BenchmarkResult> allResults;
  double baselineSpeed = 0;

  struct NestedSM      : BenchmarkInstance, HSM<model1,      NestedSM> {};
  struct NestedEntrySM : BenchmarkInstance, HSM<model1a,     NestedEntrySM> {};
  struct NestedActSM   : BenchmarkInstance, HSM<model1b,     NestedActSM> {};
  struct NestedFullSM  : BenchmarkInstance, HSM<model1c,     NestedFullSM> {};
  struct NestedEffSM   : BenchmarkInstance, HSM<model1d,     NestedEffSM> {};
  struct DeepSM        : BenchmarkInstance, HSM<modelDeep,   DeepSM> {};
  struct CrossSM       : BenchmarkInstance, HSM<modelCross,  CrossSM> {};

  allResults.push_back(runBenchmark<NestedSM, BenchmarkInstance, ToChild2, ToChild1>(
      "1. Nested states (no entry/exit/activity)", 1000, 10000,
      &baselineSpeed));

  allResults.push_back(runBenchmark<NestedEntrySM, BenchmarkInstance, ToChild2, ToChild1>(
      "1.a With entry", 1000, 10000, &baselineSpeed));

  allResults.push_back(runBenchmark<NestedActSM, BenchmarkInstance, ToChild2, ToChild1>(
      "1.b With entry+activity", 1000, 10000, &baselineSpeed));

  allResults.push_back(runBenchmark<NestedFullSM, BenchmarkInstance, ToChild2, ToChild1>(
      "1.c With entry+exit+activity", 1000, 10000, &baselineSpeed));

  allResults.push_back(runBenchmark<NestedEffSM, BenchmarkInstance, ToChild2, ToChild1>(
      "1.d With entry+exit+activity+effect", 1000, 10000, &baselineSpeed));

  allResults.push_back(
      runBenchmark<DeepSM, BenchmarkInstance,
                   ToLevel3b, ToLevel3a>("Deep nesting", 1000, 10000));

  allResults.push_back(
      runBenchmark<CrossSM, BenchmarkInstance,
                   ToParent2, ToParent1>("Cross hierarchy", 1000, 10000));

  writeResultsToCSV(allResults, "hsm_benchmark_results.csv");
  writeResultsToJSON(allResults, "hsm_benchmark_results.json");

  return 0;
}
