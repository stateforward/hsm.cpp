# Integrating hsm with FreeRTOS

`hsm` uses a **thread-free cooperative scheduling** model based on C++20 coroutines. This makes it ideal for FreeRTOS environments where you want predictable timing, minimal memory overhead, and ISR-safe event dispatch.

## Architecture Overview

Unlike traditional threading models, hsm does **not** spawn a FreeRTOS task for each timer or activity. Instead:

1. **Coroutines suspend** when waiting for timers/deadlines
2. **A single engine task** drives all coroutines by calling `resume()`
3. **Task notifications** wake the engine when events arrive
4. **Native tick counting** avoids expensive chrono conversions

This results in:
- **Minimal RAM**: One FreeRTOS task + coroutine frames (typically 64-128 bytes each)
- **Predictable latency**: No context switches for timer checks
- **ISR-safe dispatch**: Events can be queued from interrupts
- **Fewer tasks when grouped**: A single GroupTask driver can run many machines with one stack

## FreeRTOS-Optimized Types

### 1. FreeRTOSClock (Native Tick-Based)

For maximum efficiency, use native FreeRTOS ticks instead of milliseconds:

```cpp
#include "FreeRTOS.h"
#include "task.h"
#include <chrono>
#include <cstdint>

struct FreeRTOSClock {
    // Use native tick type for zero-overhead timing
    using rep = TickType_t;
    using period = std::ratio<1, configTICK_RATE_HZ>;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<FreeRTOSClock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept {
        return time_point(duration(xTaskGetTickCount()));
    }

    // ISR-safe variant
    static time_point now_from_isr() noexcept {
        return time_point(duration(xTaskGetTickCountFromISR()));
    }

    // Convert std::chrono durations to ticks
    template<typename Rep, typename Period>
    static constexpr TickType_t to_ticks(std::chrono::duration<Rep, Period> d) {
        // Use pdMS_TO_TICKS for milliseconds, direct conversion for others
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d);
        return pdMS_TO_TICKS(ms.count());
    }

    // Convert ticks to std::chrono duration
    template<typename Duration>
    static constexpr Duration to_duration(TickType_t ticks) {
        auto ms = (ticks * 1000) / configTICK_RATE_HZ;
        return std::chrono::duration_cast<Duration>(
            std::chrono::milliseconds(ms)
        );
    }
};
```

**Why native ticks?**
- `xTaskGetTickCount()` is a single register read (~2-5 cycles)
- No integer division or multiplication in the hot path
- Deadline comparisons are simple integer comparisons

### 2. FreeRTOSSignal (With Task Notification)

The signal sets an atomic flag and wakes the engine task via FreeRTOS notification:

```cpp
#include <atomic>

struct FreeRTOSSignal {
    TaskHandle_t owner_task_{nullptr};  // Task to wake on set()
    std::atomic<bool> flag_{false};
    FreeRTOSSignal* parent_{nullptr};

    // Bind to the task that will be driving this signal
    void bind(TaskHandle_t task) noexcept {
        owner_task_ = task;
    }

    // Set the flag and immediately wake the driver
    void set() noexcept {
        flag_.store(true, std::memory_order_release);

        if (owner_task_) {
            if (xPortIsInsideInterrupt()) {
                BaseType_t woken = pdFALSE;
                vTaskNotifyGiveFromISR(owner_task_, &woken);
                portYIELD_FROM_ISR(woken);
            } else {
                xTaskNotifyGive(owner_task_);
            }
        }
    }

    // Check if cancelled (including parent chain)
    [[nodiscard]] bool is_set() const noexcept {
        if (flag_.load(std::memory_order_acquire)) return true;
        if (parent_) return parent_->is_set();
        return false;
    }

    // Reset for reuse
    void reset(FreeRTOSSignal* parent = nullptr) noexcept {
        flag_.store(false, std::memory_order_release);
        parent_ = parent;
    }
};
```

**Memory footprint**: ~8-12 bytes (1 handle + 1 bool + 1 pointer on 32-bit)

### 3. FreeRTOSAwaitable

Same interface as standard `hsm::Awaitable`, but defaults to `FreeRTOSClock`:

```cpp
template <typename ClockType = FreeRTOSClock>
struct FreeRTOSAwaitable {
    using time_point = typename ClockType::time_point;
    using duration = typename ClockType::duration;

    std::optional<time_point> deadline_;
    FreeRTOSSignal* signal_{nullptr};

    // Construct with duration (relative)
    explicit FreeRTOSAwaitable(duration d, FreeRTOSSignal* sig = nullptr)
        : deadline_(ClockType::now() + d), signal_(sig) {}

    // Construct with time_point (absolute)
    explicit FreeRTOSAwaitable(time_point t, FreeRTOSSignal* sig = nullptr)
        : deadline_(t), signal_(sig) {}

    // Construct with optional deadline
    explicit FreeRTOSAwaitable(std::optional<time_point> t, FreeRTOSSignal* sig = nullptr)
        : deadline_(t), signal_(sig) {}

    // Default: no deadline, just signal
    explicit FreeRTOSAwaitable(FreeRTOSSignal* sig)
        : deadline_(std::nullopt), signal_(sig) {}

    bool await_ready() const noexcept {
        // Fast path: signal already set
        if (signal_ && signal_->is_set()) return true;
        // Fast path: deadline already passed
        if (deadline_ && ClockType::now() >= *deadline_) return true;
        return false;
    }

    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> h) const noexcept {
        // Store deadline in promise for engine to query
        if constexpr (requires { h.promise().deadline_ = deadline_; }) {
            h.promise().deadline_ = deadline_;
        }
    }

    void await_resume() const noexcept {}
};
```

### 4. FreeRTOSTask (Self-Driving await())

The key optimization: `await()` becomes the engine driver, blocking efficiently on FreeRTOS notifications:

```cpp
template <typename ClockType = FreeRTOSClock>
struct FreeRTOSTask {
    using time_point = typename ClockType::time_point;
    using duration = typename ClockType::duration;

    struct promise_type {
        std::coroutine_handle<> continuation_{nullptr};
        std::optional<time_point> deadline_;
        bool started_{false};

        FreeRTOSTask get_return_object() noexcept {
            return FreeRTOSTask{
                std::coroutine_handle<promise_type>::from_promise(*this)
            };
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept {
            struct FinalAwaiter {
                std::coroutine_handle<>* continuation;

                bool await_ready() noexcept { return false; }

                std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
                    if (continuation && *continuation) {
                        return *continuation;
                    }
                    return std::noop_coroutine();
                }

                void await_resume() noexcept {}
            };
            return FinalAwaiter{&continuation_};
        }

        void return_void() noexcept {}

        void unhandled_exception() {
            configASSERT(false);  // Halt on exception (no exceptions in embedded)
        }
    };

    std::coroutine_handle<promise_type> handle_{nullptr};

    FreeRTOSTask() noexcept = default;
    explicit FreeRTOSTask(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

    // Move-only
    FreeRTOSTask(const FreeRTOSTask&) = delete;
    FreeRTOSTask& operator=(const FreeRTOSTask&) = delete;

    FreeRTOSTask(FreeRTOSTask&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    FreeRTOSTask& operator=(FreeRTOSTask&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~FreeRTOSTask() {
        if (handle_) handle_.destroy();
    }

    [[nodiscard]] bool done() const noexcept {
        return !handle_ || handle_.done();
    }

    bool resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
            return !handle_.done();
        }
        return false;
    }

    /// Block-wait driver: becomes the engine loop
    /// CPU sleeps or switches tasks while waiting
    void await() {
        while (!done()) {
            auto d = deadline();
            TickType_t ticks = portMAX_DELAY;

            if (d) {
                // Convert deadline to ticks remaining
                auto now = ClockType::now();
                if (*d > now) {
                    // Duration until deadline
                    auto remaining = *d - now;
                    auto remaining_ticks = static_cast<std::uint64_t>(remaining.count());

                    // Clamp to avoid overflow (TickType_t is often 32-bit)
                    // Use portMAX_DELAY - 1 so we wake periodically and re-check
                    // rather than sleeping forever
                    constexpr auto max_ticks = static_cast<std::uint64_t>(portMAX_DELAY - 1);
                    ticks = static_cast<TickType_t>(std::min(remaining_ticks, max_ticks));
                } else {
                    ticks = 0;  // Deadline already passed
                }
            }

            // BLOCK here. CPU goes to sleep or switches tasks.
            // Returns when:
            // 1. Timeout expires (deadline reached)
            // 2. Signal::set() calls xTaskNotifyGive
            ulTaskNotifyTake(pdTRUE, ticks);

            // Resume the engine to process the event or timer
            resume();
        }
    }

    void start() {
        if (handle_ && !handle_.promise().started_) {
            handle_.promise().started_ = true;
            handle_.resume();
        }
    }

    [[nodiscard]] std::optional<time_point> deadline() const noexcept {
        if (handle_) {
            return handle_.promise().deadline_;
        }
        return std::nullopt;
    }

    // Awaitable interface (for co_await on nested tasks)
    bool await_ready() const noexcept { return done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        if (handle_) {
            handle_.promise().continuation_ = awaiting;
            if (!handle_.promise().started_) {
                handle_.promise().started_ = true;
            }
            return handle_;
        }
        return awaiting;
    }

    void await_resume() noexcept {}
};
```

**Key insight**: `await()` is now the blocking driver. When called from a FreeRTOS task, it:
1. Queries the coroutine's next deadline
2. Blocks with `ulTaskNotifyTake()` - CPU sleeps/switches to other tasks
3. Wakes on timeout OR notification (from `Signal::set()` or `dispatch()`)
4. Resumes the coroutine to process ready timers/events
5. Repeats until the engine terminates

## Using with HSM

Define your HSM with FreeRTOS types:

```cpp
#include "hsm/hsm.hpp"

// Your model
constexpr auto model = hsm::define("MyMachine",
    hsm::initial(hsm::target("/MyMachine/Idle")),
    hsm::state("Idle",
        hsm::transition(hsm::on_timeout([]{ return std::chrono::seconds(5); }),
                          hsm::target("/MyMachine/Active"))),
    hsm::state("Active")
);

// Your instance type
struct MyInstance {
    int counter = 0;
};

// HSM with FreeRTOS types
struct MyMachine : hsm::HSM<model, MyMachine,
                              FreeRTOSClock,
                              hsm::DefaultQueuePolicy<model>,
                              FreeRTOSTask<FreeRTOSClock>,
                              FreeRTOSAwaitable<FreeRTOSClock>,
                              FreeRTOSSignal> {
    // Your data
    int counter = 0;
};
```

## Engine Task Patterns

### Pattern 1: Dedicated Engine Task (Recommended)

With `await()` as the driver, the engine task becomes trivial:

```cpp
// Global or static HSM instance
static MyMachine g_hsm;
static TaskHandle_t g_engine_task;

void hsm_engine_task(void* pvParameters) {
    auto* hsm = static_cast<MyMachine*>(pvParameters);

    // Bind the wake signal to this task
    hsm->wake_signal_.bind(xTaskGetCurrentTaskHandle());

    // Start and drive the engine - await() handles everything
    auto engine = hsm->start();
    engine.await();  // Blocks efficiently, CPU sleeps between events

    vTaskDelete(nullptr);
}

void start_hsm() {
    xTaskCreate(
        hsm_engine_task,
        "HSM_Engine",
        configMINIMAL_STACK_SIZE * 4,  // Adjust based on coroutine depth
        &g_hsm,
        tskIDLE_PRIORITY + 2,
        &g_engine_task
    );
}
```

**That's it.** The `await()` loop inside `FreeRTOSTask`:
1. Queries the deadline from the coroutine
2. Blocks with `ulTaskNotifyTake(pdTRUE, ticks)` - CPU sleeps
3. Wakes on timeout (deadline) OR notification (event dispatch)
4. Resumes the coroutine to process timers/events
5. Repeats until the engine terminates

### Pattern 2: GroupTask Driver (Fewer Tasks / Smaller Stack Budget)

If you have many small machines, group them and drive the whole set with a single FreeRTOS task. This cuts the number of tasks (and stack allocations) from **N** to **1**.

```cpp
static MachineA m1;
static MachineB m2;
static MachineC m3;

static auto group = hsm::make_group(m1, m2, m3);

void group_engine_task(void* pvParameters) {
    (void)pvParameters;
    auto this_task = xTaskGetCurrentTaskHandle();

    // Bind all wake signals to the same driver task
    m1.wake_signal_.bind(this_task);
    m2.wake_signal_.bind(this_task);
    m3.wake_signal_.bind(this_task);

    // Start all machines and get a GroupTask driver
    auto driver = group.start();

    while (true) {
        TickType_t min_wait = portMAX_DELAY;

        if (auto dl = driver.deadline()) {
            auto now = FreeRTOSClock::now();
            if (*dl > now) {
                min_wait = static_cast<TickType_t>((*dl - now).count());
            } else {
                min_wait = 0;
            }
        }

        ulTaskNotifyTake(pdTRUE, min_wait);
        driver.resume();
    }
}
```

**Why it helps**:
1. One FreeRTOS task instead of one per machine
2. One stack to size and maintain
3. Same ISR-safe dispatch behavior (notifications wake the shared driver)

### Pattern 3: Shared Engine Task (Memory Efficient)

One FreeRTOS task drives multiple HSMs:

```cpp
template<typename... HSMs>
void multi_hsm_engine_task(void* pvParameters) {
    auto* hsms = static_cast<std::tuple<HSMs*...>*>(pvParameters);
    auto this_task = xTaskGetCurrentTaskHandle();

    // Bind all HSM wake signals to this task
    std::apply([this_task](auto*... hsm) {
        (hsm->wake_signal_.bind(this_task), ...);
    }, *hsms);

    // Start all engines
    auto engines = std::apply([](auto*... hsm) {
        return std::make_tuple(hsm->start()...);
    }, *hsms);

    // Drive all engines - find earliest deadline across all
    while (true) {
        TickType_t min_wait = portMAX_DELAY;

        std::apply([&min_wait](auto&... engine) {
            auto check_deadline = [&min_wait](auto& eng) {
                if (auto dl = eng.deadline()) {
                    auto now = FreeRTOSClock::now();
                    if (*dl > now) {
                        auto ticks = static_cast<TickType_t>((*dl - now).count());
                        min_wait = std::min(min_wait, ticks);
                    } else {
                        min_wait = 0;
                    }
                }
            };
            (check_deadline(engine), ...);
        }, engines);

        ulTaskNotifyTake(pdTRUE, min_wait);

        std::apply([](auto&... engine) {
            (engine.resume(), ...);
        }, engines);
    }
}
```

## ISR-Safe Event Dispatch

hsm's `dispatch()` is **unconditionally ISR-safe**. All dispatch calls enqueue the event and wake the engine via `wake_signal_.set()`—no special wrapper or configuration needed. **The same `dispatch()` API works identically from task context and ISR context.**

```cpp
// In ISR - safe because dispatch() always enqueues + wakes
void EXTI0_IRQHandler(void) {
    // Clear interrupt flag...

    // Safe to dispatch from ISR - no special handling needed
    g_hsm.dispatch(ButtonPressed{});
}
```

**Why this works**:
1. `dispatch()` always enqueues to the lock-free queue (uses `seq_cst` atomics)
2. After enqueueing, it calls `wake_signal_.set()`
3. The signal's `set()` method detects `xPortIsInsideInterrupt()` and uses `vTaskNotifyGiveFromISR()` automatically

No wrapper class or special ISR dispatch method is required - the same `dispatch()` call works identically from task context and ISR context.

## Memory Optimization

### Compile-Time Pool Sizing (Built-In)

The HSM already computes pool sizes at compile time from your model:

```cpp
// These are computed automatically from your model definition
static constexpr std::size_t total_timer_count = /* from model */;
static constexpr std::size_t total_activity_count = /* from model */;

// Arrays are sized exactly - no waste
std::array<std::optional<TimerActiveTask>, total_timer_count> active_timer_tasks_{};
std::array<std::optional<ActiveTask>, total_activity_count> active_tasks_{};
```

**You don't need to configure this** - the HSM analyzes your model and allocates exactly the right number of slots. Zero runtime overhead, zero wasted memory.

### Static Coroutine Frame Allocation (Built-In)

**Good news: this is now built into HSM.** The HSM automatically uses a fixed-size pool (`hsm::PromisePool`) for coroutine frame allocation—no heap allocations at runtime.

The pool is:
- **Sized at compile time** from your model's timer and activity count
- **O(1) allocation and deallocation** using a free list
- **Deferred initialization** to `start()` to avoid static init issues
- **Per-HSM instance** (no locking needed)

**Default configuration:**
- `FrameSize = 256` bytes per coroutine frame
- Pool size = `1 + 2 * (total_timers + total_activities)` (buffer for transitions)

**Customizing frame size:**

If you get assertion failures about frame size, increase the `FrameSize` template parameter:

```cpp
struct MyMachine : hsm::HSM<led_model, MyMachine,
                              FreeRTOSClock,
                              hsm::DefaultQueuePolicy<led_model>,
                              FreeRTOSTask<FreeRTOSClock>,
                              FreeRTOSAwaitable<FreeRTOSClock>,
                              FreeRTOSSignal,
                              512>  // FrameSize = 512 bytes
{ };
```

**Custom pool allocator (advanced):**

If you need thread-safe allocation (e.g., multiple HSMs driven from different tasks), provide your own pool type:

```cpp
template<std::size_t FrameSize, std::size_t PoolSize>
struct ThreadSafePool {
    alignas(std::max_align_t) std::byte storage_[FrameSize * PoolSize];
    void* free_head_{nullptr};
    portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

    // Required: static deallocate_impl for type-erased deallocation
    static void deallocate_impl(void* pool_ptr, void* raw) noexcept {
        static_cast<ThreadSafePool*>(pool_ptr)->deallocate(raw);
    }

    ThreadSafePool() noexcept {
        // Thread free list
        for (std::size_t i = 0; i < PoolSize - 1; ++i) {
            void* current = &storage_[i * FrameSize];
            void* next = &storage_[(i + 1) * FrameSize];
            *reinterpret_cast<void**>(current) = next;
        }
        *reinterpret_cast<void**>(&storage_[(PoolSize - 1) * FrameSize]) = nullptr;
        free_head_ = &storage_[0];
    }

    void* allocate(std::size_t size) noexcept {
        configASSERT(size <= FrameSize);
        portENTER_CRITICAL(&lock_);
        configASSERT(free_head_ != nullptr);
        void* result = free_head_;
        free_head_ = *reinterpret_cast<void**>(free_head_);
        portEXIT_CRITICAL(&lock_);
        return result;
    }

    void deallocate(void* ptr) noexcept {
        if (ptr) {
            portENTER_CRITICAL(&lock_);
            *reinterpret_cast<void**>(ptr) = free_head_;
            free_head_ = ptr;
            portEXIT_CRITICAL(&lock_);
        }
    }
};

// Use custom pool
struct MyMachine : hsm::HSM<led_model, MyMachine,
                              FreeRTOSClock,
                              hsm::DefaultQueuePolicy<led_model>,
                              FreeRTOSTask<FreeRTOSClock>,
                              FreeRTOSAwaitable<FreeRTOSClock>,
                              FreeRTOSSignal,
                              256,
                              ThreadSafePool>
{ };
```

### Stack Size Estimation

Coroutines use minimal stack since they suspend. Typical requirements:

| Component | Stack Usage |
|-----------|-------------|
| Engine task base | 256-512 bytes |
| Per coroutine frame | 64-128 bytes |
| Behavior execution | 128-256 bytes |
| **Recommended total** | **1024-2048 bytes** |

## Performance Characteristics

| Operation | Cycles (Cortex-M4) | Notes |
|-----------|-------------------|-------|
| `Signal::set()` | ~10 | Atomic store |
| `Signal::is_set()` | ~15-30 | Atomic load + parent check |
| `FreeRTOSClock::now()` | ~5 | Single register read |
| `Awaitable::await_ready()` | ~20-40 | Signal + time compare |
| `Task::resume()` | ~50-100 | Coroutine resume |
| `xTaskNotifyGive()` | ~50-100 | FreeRTOS notification |
| `ulTaskNotifyTake()` | ~100-200 | With context switch |

### Latency Analysis

- **Event dispatch to handler**: ~1-5 µs (same task) or ~10-50 µs (cross-task with notification)
- **Timer firing accuracy**: ±1 tick (typically 1ms with 1kHz tick rate)
- **ISR to handler**: ~20-100 µs (depends on task priorities)

## Complete Example

```cpp
#include "FreeRTOS.h"
#include "task.h"
#include "hsm/hsm.hpp"

// FreeRTOS types (as defined above)
// ... FreeRTOSClock, FreeRTOSSignal, FreeRTOSAwaitable, FreeRTOSTask ...

// Events
struct ButtonPressed : hsm::Event<hsm::event_kind("ButtonPressed")> {};
struct Timeout : hsm::Event<hsm::event_kind("Timeout")> {};

// Model
constexpr auto led_model = hsm::define("LED",
    hsm::initial(hsm::target("/LED/Off")),

    hsm::state("Off",
        hsm::transition(hsm::on<ButtonPressed>(),
                          hsm::target("/LED/On"))),

    hsm::state("On",
        hsm::transition(hsm::on<ButtonPressed>(),
                          hsm::target("/LED/Off")),
        // Auto-off after 5 seconds
        hsm::transition(hsm::on_timeout([]{ return std::chrono::seconds(5); }),
                          hsm::target("/LED/Off")))
);

// Machine
struct LEDMachine : hsm::HSM<led_model, LEDMachine,
                               FreeRTOSClock,
                               hsm::DefaultQueuePolicy<led_model>,
                               FreeRTOSTask<FreeRTOSClock>,
                               FreeRTOSAwaitable<FreeRTOSClock>,
                               FreeRTOSSignal> {
    void set_led(bool on) {
        // HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
    }
};

// Global instance
static LEDMachine g_led;
static TaskHandle_t g_led_engine;

void led_engine_task(void* pvParameters) {
    auto* led = static_cast<LEDMachine*>(pvParameters);

    // Bind wake signal so dispatch() can wake us
    led->wake_signal_.bind(xTaskGetCurrentTaskHandle());

    // Start and block-drive the engine
    auto engine = led->start();
    engine.await();  // CPU sleeps between events/timers

    vTaskDelete(nullptr);
}

// Button ISR - dispatch handles notification via wake_signal_.set()
void EXTI0_IRQHandler(void) {
    HAL_GPIO_EXTI_IRQHandler(BUTTON_Pin);

    // Safe from ISR - Signal::set() handles vTaskNotifyGiveFromISR
    g_led.dispatch(ButtonPressed{});
}

int main() {
    // HAL init...

    xTaskCreate(led_engine_task, "LED", 1024, &g_led, 2, &g_led_engine);
    vTaskStartScheduler();

    for (;;) {}
}
```

## Configuration Checklist

1. **FreeRTOSConfig.h**:
   ```c
   #define configUSE_TASK_NOTIFICATIONS 1
   #define configTICK_RATE_HZ 1000  // 1ms tick for accurate timing
   ```

2. **Compiler flags**:
   ```
   -std=c++20 -fcoroutines
   ```

3. **Memory**:
   - Allocate 1-2KB stack per engine task
   - Size coroutine pool based on max concurrent timers + activities

4. **Priorities**:
   - Engine task: `tskIDLE_PRIORITY + 2` or higher
   - ISR sources: Standard peripheral priority

## Troubleshooting

| Issue | Cause | Solution |
|-------|-------|----------|
| Timers not firing | Engine task not running | Check task creation and priority |
| Events lost | Queue overflow | Increase `QueuePolicy::capacity` |
| Deadlocks | Wrong task notification usage | Use `ulTaskNotifyTake(pdTRUE, ...)` |
| Stack overflow | Deep coroutine nesting | Increase engine task stack |
| ISR dispatch fails | Notification not sent | Add `vTaskNotifyGiveFromISR` call |
