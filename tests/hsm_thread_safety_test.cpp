#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "hsm/hsm.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace hsm;

// ============================================================================
// Gap #17: Thread Safety Tests
//
// enqueue_event (hsm.hpp:3830-3847) uses load/store on queue_tail_, NOT
// fetch_add. The comment at line 3831 explicitly states: "concurrent producers
// must provide external serialization." So dispatch() is single-producer only.
//
// Tests:
// 1. Single producer + engine consumer (documented use case)
// 2. Multi-producer stress test (documents the boundary)
// 3. Queue pressure is not surfaced through dispatch return values
// ============================================================================

// Events (IDs 500-509)
struct Ping : Event<make_kind(500, Kind::Event)> {};

// ---------------------------------------------------------------------------
// Simple A↔B toggle on Ping with atomic transition counter
// ---------------------------------------------------------------------------

struct ThreadCtx {
    std::atomic<int> transition_count{0};
};

constexpr auto thread_model = define("TH",
    initial(target("/TH/A")),
    state("A",
        transition(on<Ping>(),
                   effect([](ThreadCtx& c) { c.transition_count.fetch_add(1, std::memory_order_relaxed); }),
                   target("/TH/B"))),
    state("B",
        transition(on<Ping>(),
                   effect([](ThreadCtx& c) { c.transition_count.fetch_add(1, std::memory_order_relaxed); }),
                   target("/TH/A"))));

struct ThreadSM : ThreadCtx, HSM<thread_model, ThreadSM> {};

// ---------------------------------------------------------------------------
// Test 1: Single producer, engine consumer — no data loss
// ---------------------------------------------------------------------------

TEST_CASE("thread safety - single producer, engine consumer") {
    constexpr int N = 2;

    // Heap-allocate to outlive the engine thread (same pattern as bench)
    auto* sm = new ThreadSM();

    // Engine thread: runs the event loop
    std::thread engine([sm]() {
        sm->start().await();
    });

    // Give engine time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Single producer dispatches within bounded queue capacity. Public dispatch
    // no longer reports queue pressure, so this path stays below that boundary.
    for (int i = 0; i < N; ++i) {
        sm->dispatch(Ping{});
    }

    // Spin-wait until all events are processed (with timeout)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        count = sm->transition_count.load(std::memory_order_acquire);
        if (count >= N) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(count == N);

    // Engine thread has no graceful shutdown; detach
    engine.detach();
    // sm intentionally leaked to avoid use-after-free
}

// ---------------------------------------------------------------------------
// Test 2: Multi-producer stress test (documents boundary)
// ---------------------------------------------------------------------------

TEST_CASE("thread safety - multi-producer stress test") {
    constexpr int THREADS = 4;
    constexpr int PER_THREAD = 500;

    auto* sm = new ThreadSM();

    std::thread engine([sm]() {
        sm->start().await();
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Launch multiple producer threads
    std::vector<std::thread> producers;
    for (int t = 0; t < THREADS; ++t) {
        producers.emplace_back([sm]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                sm->dispatch(Ping{});
            }
        });
    }

    for (auto& t : producers) {
        t.join();
    }

    // Spin-wait until at least some events are processed (with timeout)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int count = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        count = sm->transition_count.load(std::memory_order_acquire);
        if (count > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Multi-producer exceeds the documented single-producer contract.
    // We don't guarantee all events are processed, but the SM must not
    // crash and must end in a valid state.
    std::string st = std::string(sm->state());
    bool valid_state = (st == "/TH/A" || st == "/TH/B");
    CHECK(valid_state);

    // At least some events should have been processed
    CHECK(count > 0);

    // Document: with single-producer guarantee, count would equal
    // THREADS * PER_THREAD. Without it, some events may be lost.
    MESSAGE("multi-producer: processed " << count << " / "
            << (THREADS * PER_THREAD) << " events");

    engine.detach();
}

// ---------------------------------------------------------------------------
// Test 3: Queue pressure is not a dispatch result
// ---------------------------------------------------------------------------

TEST_CASE("thread safety - queue pressure has no public dispatch result") {
    ThreadSM sm;
    // Do NOT start the engine — queue is not being drained.
    // We still need to call start() to initialize the SM, but we won't
    // run the engine loop. Use the fire-and-forget pattern.
    auto task = sm.start();
    CHECK(sm.state() == "/TH/A");

    // Flood the queue without running the engine. Overflow is intentionally not
    // observable through dispatch's return type.
    for (int i = 0; i < 10000; ++i) {
        sm.dispatch(Ping{});
    }

    CHECK(sm.state() == "/TH/A");
}
