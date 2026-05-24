#include <iostream>
#include <chrono>
#include <cstdlib>
#include <sys/resource.h>
#include <string>
#include <vector>

#include "hsm/hsm.hpp"

struct TrafficLight {
    bool maintenance_mode = false;
    int cars_waiting = 0;
    int timer = 0;
};

struct TimerEvent : hsm::Event<hsm::event_kind("TimerEvent")> {};
struct CarArrival : hsm::Event<hsm::event_kind("CarArrival")> {};
struct PedestrianButton : hsm::Event<hsm::event_kind("PedestrianButton")> {};
struct MaintenanceSwitch : hsm::Event<hsm::event_kind("MaintenanceSwitch")> {};
struct Tick : hsm::Event<hsm::event_kind("Tick")> {};

static void reset_cars(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { inst.cars_waiting = 0; }
static void add_car(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { inst.cars_waiting++; }
static bool no_cars_waiting(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { return inst.cars_waiting == 0; }
static bool is_maintenance(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { return inst.maintenance_mode; }
static bool is_not_maintenance(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { return !inst.maintenance_mode; }
static bool check_cars_for_choice(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { return inst.cars_waiting > 10; }
static void set_timer_extended(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { inst.timer = 60; }
static void set_timer_standard(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { inst.timer = 40; }
static void maintenance_tick(hsm::Signal&, TrafficLight& inst, const hsm::EventBase&) { inst.timer++; }

using namespace hsm;

constexpr auto model = define("TrafficLight",
    initial(target("/TrafficLight/operational")),
    
    state("operational",
        transition(on<MaintenanceSwitch>(), guard(is_maintenance), target("/TrafficLight/maintenance")),
        initial(target("/TrafficLight/operational/red")),
        
        state("red",
            transition(on<TimerEvent>(), guard(check_cars_for_choice), effect(set_timer_extended), target("/TrafficLight/operational/green")),
            transition(on<TimerEvent>(), effect(set_timer_standard), target("/TrafficLight/operational/green")),
            transition(on<CarArrival>(), effect(add_car))
        ),
        
        state("green",
            transition(on<TimerEvent>(), target("/TrafficLight/operational/yellow")),
            transition(on<PedestrianButton>(), guard(no_cars_waiting), target("/TrafficLight/operational/yellow"))
        ),
        
        state("yellow",
            defer<CarArrival>(),
            transition(on<TimerEvent>(), target("/TrafficLight/operational/red"))
        )
    ),
    
    state("maintenance",
        entry(reset_cars),
        transition(on<Tick>(), effect(maintenance_tick)),
        transition(on<MaintenanceSwitch>(), guard(is_not_maintenance), target("/TrafficLight/operational"))
    )
);

struct TrafficLightSM : TrafficLight, HSM<model, TrafficLightSM> {};

double get_memory_mb() {
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
    return usage.ru_maxrss / (1024.0 * 1024.0);
#else
    return usage.ru_maxrss / 1024.0;
#endif
}

template <typename T>
inline void doNotOptimize(T const& value) {
    asm volatile("" : : "m"(value) : "memory");
}

std::size_t env_or_default(const char* name, std::size_t default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return default_value;
    }
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || parsed == 0) {
        return default_value;
    }
    return static_cast<std::size_t>(parsed);
}

void dispatch_batch(TrafficLightSM& sm, std::size_t cycles) {
    for (std::size_t i = 0; i < cycles; ++i) {
        sm.process<CarArrival>();
        sm.process<TimerEvent>();
        sm.process<TimerEvent>();
        sm.process<TimerEvent>();
    }
}

std::size_t calibrate_batch(TrafficLightSM& sm) {
    constexpr double target_batch_ms = 10.0;
    std::size_t cycles = 1;
    while (true) {
        auto start_time = std::chrono::steady_clock::now();
        dispatch_batch(sm, cycles);
        double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start_time
        ).count();
        if (elapsed_ms >= target_batch_ms || cycles >= (1u << 24)) {
            return cycles;
        }
        cycles *= 2;
    }
}

int main() {
    const std::size_t warmup_ms = env_or_default("HSM_BENCH_WARMUP_MS", 250);
    const std::size_t duration_target_ms = env_or_default("HSM_BENCH_DURATION_MS", 2000);

    TrafficLightSM warmup_sm;
    warmup_sm.start();
    const std::size_t batch_cycles = calibrate_batch(warmup_sm);
    auto warmup_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
    while (std::chrono::steady_clock::now() < warmup_deadline) {
        dispatch_batch(warmup_sm, batch_cycles);
    }
    
    TrafficLightSM sm;
    sm.start();
    
    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + std::chrono::milliseconds(duration_target_ms);
    std::size_t completed_cycles = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        dispatch_batch(sm, batch_cycles);
        completed_cycles += batch_cycles;
    }
    auto end_time = std::chrono::steady_clock::now();
    doNotOptimize(sm);
    
    double duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    std::size_t total_dispatches = completed_cycles * 4;
    std::size_t ops_per_sec = static_cast<std::size_t>((total_dispatches / duration_ms) * 1000.0);
    
    std::cout << "{"
              << "\"language\": \"C++\", "
              << "\"iterations\": " << total_dispatches << ", "
              << "\"duration_ms\": " << duration_ms << ", "
              << "\"memory_mb\": " << get_memory_mb() << ", "
              << "\"throughput_ops_per_sec\": " << ops_per_sec
              << "}\n";
              
    return 0;
}
