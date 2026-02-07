#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

#include "hsm/hsm.hpp"

// Configuration flags handled by CMake
// HSM_BENCH_ENABLE_HSMCPP
// HSM_BENCH_ENABLE_BOOST_MSM
// HSM_BENCH_ENABLE_QP
// HSM_BENCH_ENABLE_SML
// HSM_BENCH_ENABLE_HFSM2

// --- HSM Models ---
namespace backend_hsm {
using namespace hsm;
struct BenchInstance {
  bool guard_flag = false;
  std::size_t traffic_entry_count = 0;
  std::size_t traffic_exit_count = 0;
};

struct E1 : hsm::Event<hsm::make_kind(1, hsm::Kind::Event)> {};
struct E2 : hsm::Event<hsm::make_kind(2, hsm::Kind::Event)> {};
struct G  : hsm::Event<hsm::make_kind(3, hsm::Kind::Event)> {};
struct T  : hsm::Event<hsm::make_kind(4, hsm::Kind::Event)> {};

static void noop_behavior(Signal &, BenchInstance &, const EventBase &) {}
static void traffic_entry(Signal &, BenchInstance &inst, const EventBase &) { ++inst.traffic_entry_count; }
static void traffic_exit(Signal &, BenchInstance &inst, const EventBase &) { ++inst.traffic_exit_count; }

static constexpr auto model_ping_pong = define("PingPong",
    state("A"), state("B"),
    transition(on<E1>(), source("/PingPong/A"), target("/PingPong/B")),
    transition(on<E2>(), source("/PingPong/B"), target("/PingPong/A")),
    initial(target("/PingPong/A")));

static constexpr auto model_hierarchical = define("Hierarchical",
    state("P", entry(noop_behavior), exit(noop_behavior),
          state("C1", entry(noop_behavior), exit(noop_behavior)),
          state("C2", entry(noop_behavior), exit(noop_behavior)),
          initial(target("/Hierarchical/P/C1")),
          transition(on<E1>(), source("/Hierarchical/P/C1"), target("/Hierarchical/P/C2")),
          transition(on<E2>(), source("/Hierarchical/P/C2"), target("/Hierarchical/P/C1"))),
    initial(target("/Hierarchical/P")));

static constexpr auto model_deep = define("Deep",
    state("L1", entry(noop_behavior), exit(noop_behavior),
          state("L2", entry(noop_behavior), exit(noop_behavior),
                state("L3a", entry(noop_behavior), exit(noop_behavior)),
                state("L3b", entry(noop_behavior), exit(noop_behavior)),
                initial(target("/Deep/L1/L2/L3a")),
                transition(on<E1>(), source("/Deep/L1/L2/L3a"), target("/Deep/L1/L2/L3b")),
                transition(on<E2>(), source("/Deep/L1/L2/L3b"), target("/Deep/L1/L2/L3a"))),
          initial(target("/Deep/L1/L2"))),
    initial(target("/Deep/L1")));

static constexpr auto model_guarded = define("Guarded",
    state("S", transition(on<G>(), guard([](BenchInstance &, const G &){ return true; }), target("/Guarded/S"))),
    initial(target("/Guarded/S")));

static constexpr auto model_traffic = define("Traffic",
    state("Operational",
          state("NS", state("Green", entry(traffic_entry), exit(traffic_exit)), state("Yellow", entry(traffic_entry), exit(traffic_exit)), initial(target("/Traffic/Operational/NS/Green"))),
          state("EW", state("Green", entry(traffic_entry), exit(traffic_exit)), state("Yellow", entry(traffic_entry), exit(traffic_exit)), initial(target("/Traffic/Operational/EW/Green"))),
          state("AllRed1", entry(traffic_entry), exit(traffic_exit)),
          state("AllRed2", entry(traffic_entry), exit(traffic_exit)),
          transition(on<T>(), source("/Traffic/Operational/NS/Green"), target("/Traffic/Operational/NS/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/NS/Yellow"), target("/Traffic/Operational/AllRed1")),
          transition(on<T>(), source("/Traffic/Operational/AllRed1"), target("/Traffic/Operational/EW/Green")),
          transition(on<T>(), source("/Traffic/Operational/EW/Green"), target("/Traffic/Operational/EW/Yellow")),
          transition(on<T>(), source("/Traffic/Operational/EW/Yellow"), target("/Traffic/Operational/AllRed2")),
          transition(on<T>(), source("/Traffic/Operational/AllRed2"), target("/Traffic/Operational/NS/Green"))),
    initial(target("/Traffic/Operational")));

struct PingPong : BenchInstance, HSM<model_ping_pong, PingPong> {};
struct Hierarchical : BenchInstance, HSM<model_hierarchical, Hierarchical> {};
struct Deep : BenchInstance, HSM<model_deep, Deep> {};
struct Guarded : BenchInstance, HSM<model_guarded, Guarded> {};
struct Traffic : BenchInstance, HSM<model_traffic, Traffic> {};
} // namespace backend_hsm

// --- SML ---
#if defined(HSM_BENCH_ENABLE_SML)
#include <boost/sml.hpp>
namespace backend_sml {
namespace sml = boost::sml;
struct E1 {}; struct E2 {}; struct G {}; struct T {};
struct ping_pong_sm { auto operator()() const { using namespace sml; return make_transition_table(*"A"_s + event<E1> = "B"_s, "B"_s + event<E2> = "A"_s); } };
struct hierarchical_sm { auto operator()() const { using namespace sml; return make_transition_table(*"P_C1"_s + event<E1> = "P_C2"_s, "P_C2"_s + event<E2> = "P_C1"_s); } };
struct deep_sm { auto operator()() const { using namespace sml; return make_transition_table(*"L3a"_s + event<E1> = "L3b"_s, "L3b"_s + event<E2> = "L3a"_s); } };
struct guard_data { bool flag{false}; };
struct guarded_sm { auto operator()() const { using namespace sml; return make_transition_table(*"S"_s + event<G> [ ([](guard_data&){}) ] = "S"_s); } };
struct traffic_sm { auto operator()() const { using namespace sml; return make_transition_table(*"NS_Green"_s + event<T> = "NS_Yellow"_s, "NS_Yellow"_s + event<T> = "AllRed1"_s, "AllRed1"_s + event<T> = "EW_Green"_s, "EW_Green"_s + event<T> = "EW_Yellow"_s, "EW_Yellow"_s + event<T> = "AllRed2"_s, "AllRed2"_s + event<T> = "NS_Green"_s); } };

using PingPong = sml::sm<ping_pong_sm>;
using Hierarchical = sml::sm<hierarchical_sm>;
using Deep = sml::sm<deep_sm>;
using Guarded = sml::sm<guarded_sm>;
using Traffic = sml::sm<traffic_sm>;
}
#endif

// --- Boost.MSM ---
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
#include <boost/msm/front/state_machine_def.hpp>
#include <boost/msm/front/functor_row.hpp>
#include <boost/msm/back/state_machine.hpp>
#include <boost/mpl/vector.hpp>
namespace backend_msm {
namespace msm = boost::msm; namespace mpl = boost::mpl; using namespace msm::front;
struct E1 {}; struct E2 {}; struct G {}; struct T {};
struct ping_pong_ : state_machine_def<ping_pong_> {
  struct A : state<> {}; struct B : state<> {}; using initial_state = A;
  struct transition_table : mpl::vector<Row<A, E1, B>, Row<B, E2, A>> {};
};
struct hierarchical_ : state_machine_def<hierarchical_> {
  struct P_C1 : state<> {}; struct P_C2 : state<> {}; using initial_state = P_C1;
  struct transition_table : mpl::vector<Row<P_C1, E1, P_C2>, Row<P_C2, E2, P_C1>> {};
};
struct deep_ : state_machine_def<deep_> {
  struct L3a : state<> {}; struct L3b : state<> {}; using initial_state = L3a;
  struct transition_table : mpl::vector<Row<L3a, E1, L3b>, Row<L3b, E2, L3a>> {};
};
struct guarded_ : state_machine_def<guarded_> {
  struct S : state<> {}; using initial_state = S; bool flag{false};
  struct guard { template<class E, class F, class S, class T> bool operator()(E const&, F&, S&, T&) const { return true; } };
  struct transition_table : mpl::vector<Row<S, G, S, guard>> {};
};
struct operational_ : state_machine_def<operational_> {
  struct NS_Green : state<> {}; struct NS_Yellow : state<> {}; struct AllRed1 : state<> {};
  struct EW_Green : state<> {}; struct EW_Yellow : state<> {}; struct AllRed2 : state<> {};
  using initial_state = NS_Green;
  struct transition_table : mpl::vector<
    Row<NS_Green, T, NS_Yellow>, Row<NS_Yellow, T, AllRed1>, Row<AllRed1, T, EW_Green>,
    Row<EW_Green, T, EW_Yellow>, Row<EW_Yellow, T, AllRed2>, Row<AllRed2, T, NS_Green>> {};
};
using PingPong = msm::back::state_machine<ping_pong_>;
using Hierarchical = msm::back::state_machine<hierarchical_>;
using Deep = msm::back::state_machine<deep_>;
using Guarded = msm::back::state_machine<guarded_>;
using Traffic = msm::back::state_machine<operational_>;
}
#endif

// --- Vanilla ---
namespace backend_vanilla {
// Estimates
struct PingPong { int state; };
struct Hierarchical { int state; int c1; int c2; }; // Approx
}

// --- QP Stubs ---
#if defined(HSM_BENCH_ENABLE_QP)
#include <iostream>
namespace QP {
namespace QF {
void enterCriticalSection_() {}
void leaveCriticalSection_() {}
}
}
extern "C" void Q_onError(char const * const module, int const id) {
  std::cerr << "QP Error: " << (module ? module : "?") << ":" << id << "\n";
  std::terminate();
}
#endif

int main() {
    std::cout << "HSM Instance Footprint (Stack Size) [bytes]\n";
    std::cout << "===========================================\n";
    std::cout << std::left << std::setw(20) << "Scenario" 
              << std::setw(10) << "HSM"
              << std::setw(10) << "SML"
              << std::setw(10) << "MSM"
              << "\n";
              
    auto print_row = [](const std::string& name, std::size_t hsm_sz, std::size_t sml_sz, std::size_t msm_sz) {
        std::cout << std::left << std::setw(20) << name 
                  << std::setw(10) << hsm_sz
                  << std::setw(10) << (sml_sz == 0 ? "-" : std::to_string(sml_sz))
                  << std::setw(10) << (msm_sz == 0 ? "-" : std::to_string(msm_sz))
                  << "\n";
    };

    std::size_t s_hsm, s_sml, s_msm;

    // PingPong
    s_hsm = sizeof(backend_hsm::PingPong);
    s_sml = 0; s_msm = 0;
#if defined(HSM_BENCH_ENABLE_SML)
    s_sml = sizeof(backend_sml::PingPong);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    s_msm = sizeof(backend_msm::PingPong);
#endif
    print_row("PingPong", s_hsm, s_sml, s_msm);

    // Hierarchical
    s_hsm = sizeof(backend_hsm::Hierarchical);
    s_sml = 0; s_msm = 0;
#if defined(HSM_BENCH_ENABLE_SML)
    s_sml = sizeof(backend_sml::Hierarchical);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    s_msm = sizeof(backend_msm::Hierarchical);
#endif
    print_row("Hierarchical", s_hsm, s_sml, s_msm);

    // Deep
    s_hsm = sizeof(backend_hsm::Deep);
    s_sml = 0; s_msm = 0;
#if defined(HSM_BENCH_ENABLE_SML)
    s_sml = sizeof(backend_sml::Deep);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    s_msm = sizeof(backend_msm::Deep);
#endif
    print_row("Deep", s_hsm, s_sml, s_msm);

    // Guarded
    s_hsm = sizeof(backend_hsm::Guarded);
    s_sml = 0; s_msm = 0;
#if defined(HSM_BENCH_ENABLE_SML)
    s_sml = sizeof(backend_sml::Guarded);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    s_msm = sizeof(backend_msm::Guarded);
#endif
    print_row("Guarded", s_hsm, s_sml, s_msm);

    // Traffic
    s_hsm = sizeof(backend_hsm::Traffic);
    s_sml = 0; s_msm = 0;
#if defined(HSM_BENCH_ENABLE_SML)
    s_sml = sizeof(backend_sml::Traffic);
#endif
#if defined(HSM_BENCH_ENABLE_BOOST_MSM)
    s_msm = sizeof(backend_msm::Traffic);
#endif
    print_row("Traffic", s_hsm, s_sml, s_msm);

    return 0;
}
