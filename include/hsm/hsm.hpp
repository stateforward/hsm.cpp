#ifndef HSM_HSM_HPP
#define HSM_HSM_HPP

#include <any>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cmath>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "detail/behaviors.hpp"
#include "detail/expressions.hpp"
#include "detail/fixed_string.hpp"
#include "detail/hash.hpp"
#include "detail/normalize.hpp"
#include "detail/structural_tuple.hpp"
#include "kind.hpp"
#include "detail/assert.hpp"
#include "detail/hash_table.hpp"

namespace hsm {

using Kind = hsm::Kind;
using hsm::make_kind;
using Clock = std::chrono::steady_clock;

struct Signal {
  constexpr Signal() = default;
  virtual ~Signal() = default;

  Signal(const Signal &) = delete;
  Signal &operator=(const Signal &) = delete;
  Signal(Signal &&) = delete;
  Signal &operator=(Signal &&) = delete;

  virtual void set() {
    flag_.store (
      true,
      std::memory_order_release
    );
  }

  [[nodiscard]] virtual bool is_set() const {
    bool self = flag_.load (std::memory_order_acquire);
    if (self) return true;
    if (parent_) return parent_->is_set ();
    return false;
  }

  // virtual void wait() {
  //   while (!is_set ()) {
  //     std::this_thread::yield ();
  //   }
  // }

  virtual void reset(Signal *parent = nullptr) {
    flag_.store (
      false,
      std::memory_order_release
    );
    parent_ = parent;
  }

protected:
  std::atomic_bool flag_{false};
  Signal *parent_{nullptr};
};

namespace detail {
using dispatch_status_t = std::uint8_t;
inline constexpr dispatch_status_t queue_full_status = 0;
inline constexpr dispatch_status_t processed_status = 1;
inline constexpr dispatch_status_t deferred_status = 2;
}

// Header stored before each coroutine frame for type-erased deallocation
struct FrameHeader {
  void (*deallocate_fn)(void* pool, void* raw);  // Type-erased deallocate
  void* pool;  // Pool pointer (nullptr for heap allocation)
};

// PromisePool: O(1) fixed-size pool allocator for coroutine frames.
// Uses intrusive free list - no heap allocation.
// PoolSize is computed at compile time from model's timer + activity count.
template <std::size_t FrameSize, std::size_t PoolSize>
struct PromisePool {
  alignas(std::max_align_t) std::byte storage_[FrameSize * PoolSize];
  void* free_head_{nullptr};

  // Type-erased deallocate function
  static void deallocate_impl(void* pool_ptr, void* raw) noexcept {
    static_cast<PromisePool*>(pool_ptr)->deallocate(raw);
  }

  // Constructor threads the free list - O(PoolSize) once at init
  PromisePool() noexcept {
    if constexpr (PoolSize > 0) {
      for (std::size_t i = 0; i < PoolSize - 1; ++i) {
        void* current = &storage_[i * FrameSize];
        void* next = &storage_[(i + 1) * FrameSize];
        *reinterpret_cast<void**>(current) = next;
      }
      *reinterpret_cast<void**>(&storage_[(PoolSize - 1) * FrameSize]) = nullptr;
      free_head_ = &storage_[0];
    }
  }

  // O(1) allocation - pop from free list
  void* allocate(std::size_t size) noexcept {
    assert(size <= FrameSize && "Coroutine frame exceeds FrameSize");
    assert(free_head_ != nullptr && "PromisePool exhausted");
    (void)size;  // Suppress unused warning in release builds
    void* result = free_head_;
    free_head_ = *reinterpret_cast<void**>(free_head_);
    return result;
  }

  // O(1) deallocation - push to free list
  void deallocate(void* ptr) noexcept {
    if (ptr) {
      *reinterpret_cast<void**>(ptr) = free_head_;
      free_head_ = ptr;
    }
  }
};

// Helper base class for Task promise return handling
namespace detail {
template <typename Result>
struct TaskReturnHandler {
  std::optional<Result> result_;
  void return_value(Result value) noexcept { result_ = std::move(value); }
};

template <>
struct TaskReturnHandler<void> {
  void return_void() noexcept {}
};
}  // namespace detail

// Task: Unified coroutine type for HSM engine, activities, and timers.
// Supports deadline tracking for cooperative scheduling.
// Can be co_awaited (yields until done) or use join() (blocks until done).
// Template parameters:
//   ClockType - Clock for deadline tracking (default: Clock)
//   Result - Return type of the coroutine (default: void)
template <typename ClockType = Clock, typename Result = void>
struct Task {
  using clock_type = ClockType;
  using time_point = typename clock_type::time_point;
  using duration = typename clock_type::duration;
  using result_type = Result;

  struct promise_type : detail::TaskReturnHandler<Result> {
    std::coroutine_handle<> continuation_{nullptr};
    std::function<void()> on_done_callback_;
    std::optional<time_point> deadline_;
    bool started_{false};

    Task get_return_object() noexcept {
      return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
      struct FinalAwaiter {
        std::function<void()>* callback;
        std::coroutine_handle<>* continuation;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
          if (callback && *callback) {
            (*callback)();
          }
          if (continuation && *continuation) {
            return *continuation;
          }
          return std::noop_coroutine();
        }

        void await_resume() noexcept {}
      };
      return FinalAwaiter{&on_done_callback_, &continuation_};
    }

    void unhandled_exception() { std::terminate(); }

    // Pool-based allocation for member function coroutines
    // For member functions, args are: (this, pool, ...)
    // We embed a FrameHeader before the frame for type-erased deallocation
    template<typename Self, typename Pool, typename... Args>
      requires requires(Pool& p, std::size_t s) { { p.allocate(s) } -> std::same_as<void*>; }
    static void* operator new(std::size_t size, Self&, Pool& pool, Args&&...) {
      // Allocate extra space for header
      void* raw = pool.allocate(size + sizeof(FrameHeader));
      // Store header at the beginning
      auto* header = static_cast<FrameHeader*>(raw);
      header->deallocate_fn = &Pool::deallocate_impl;
      header->pool = &pool;
      // Return pointer after the header
      return static_cast<char*>(raw) + sizeof(FrameHeader);
    }

    // Fallback for coroutines without pool parameter (uses heap)
    static void* operator new(std::size_t size) {
      // Allocate extra space for header
      void* raw = ::operator new(size + sizeof(FrameHeader));
      // Store header indicating heap allocation
      auto* header = static_cast<FrameHeader*>(raw);
      header->deallocate_fn = nullptr;
      header->pool = nullptr;
      return static_cast<char*>(raw) + sizeof(FrameHeader);
    }

    // Single delete handles both pool and heap allocations
    static void operator delete(void* ptr) {
      if (!ptr) return;
      // Get the header stored before the frame
      auto* header = reinterpret_cast<FrameHeader*>(
        static_cast<char*>(ptr) - sizeof(FrameHeader));
      if (header->deallocate_fn) {
        // Pool allocation - use type-erased deallocate
        header->deallocate_fn(header->pool, header);
      } else {
        // Heap allocation
        ::operator delete(header);
      }
    }
  };

  std::coroutine_handle<promise_type> handle_{nullptr};

  Task() noexcept = default;
  explicit Task(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;

  Task(Task&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~Task() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // Check if coroutine has completed
  [[nodiscard]] bool done() const noexcept {
    return !handle_ || handle_.done();
  }

  // Check if task is joinable (has work remaining)
  [[nodiscard]] bool joinable() const noexcept {
    return handle_ && !handle_.done();
  }

  // Resume execution (returns true if more work remains)
  bool resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
      return !handle_.done();
    }
    return false;
  }

  // Blocking await - resumes until done (use from outside coroutines)
  // Example: hsm.dispatch(event).await()
  // For non-void Result, returns the result after completion.
  auto await() {
    while (!done()) {
      resume();
    }
    if constexpr (!std::is_void_v<Result>) {
      return get_result();
    }
  }

  // Get result (only for non-void Result)
  [[nodiscard]] Result get_result() const noexcept requires (!std::is_void_v<Result>) {
    return handle_.promise().result_.value();
  }

  // Start the coroutine (first resume)
  void start() {
    if (handle_ && !handle_.promise().started_) {
      handle_.promise().started_ = true;
      handle_.resume();
    }
  }

  // Get the deadline this task is waiting for (if any)
  [[nodiscard]] std::optional<time_point> deadline() const noexcept {
    if (handle_) {
      return handle_.promise().deadline_;
    }
    return std::nullopt;
  }

  // Set completion callback
  void on_done(std::function<void()> callback) {
    if (handle_) {
      handle_.promise().on_done_callback_ = std::move(callback);
    }
  }

  // Awaitable interface - allows co_await on Task
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

  // Return result when awaited (void or Result)
  auto await_resume() noexcept {
    if constexpr (std::is_void_v<Result>) {
      // void - no return
    } else {
      return get_result();
    }
  }
};

// Awaitable: Cooperative awaitable that suspends until deadline or signal.
// Stores deadline in the coroutine's promise for engine to query.
// No threads spawned - relies on external driver to resume when ready.
template <typename ClockType = Clock>
struct Awaitable {
  using clock_type = ClockType;
  using time_point = typename clock_type::time_point;
  using duration = typename clock_type::duration;

  std::optional<time_point> deadline_;
  Signal* signal_{nullptr};

  // Construct with duration (relative)
  explicit Awaitable(duration d, Signal* sig = nullptr)
    : deadline_(clock_type::now() + d), signal_(sig) {}

  // Construct with time_point (absolute)
  explicit Awaitable(time_point t, Signal* sig = nullptr)
    : deadline_(t), signal_(sig) {}

  // Construct with optional deadline
  explicit Awaitable(std::optional<time_point> t, Signal* sig = nullptr)
    : deadline_(t), signal_(sig) {}

  // Default: no deadline, just signal
  explicit Awaitable(Signal* sig)
    : deadline_(std::nullopt), signal_(sig) {}

  bool await_ready() const noexcept {
    // Ready immediately if: signal set OR deadline passed
    if (signal_ && signal_->is_set()) return true;
    if (deadline_ && clock_type::now() >= *deadline_) return true;
    return false;
  }

  template <typename Promise>
  void await_suspend(std::coroutine_handle<Promise> h) const noexcept {
    // Store deadline in the coroutine's promise for engine to query
    if constexpr (requires { h.promise().deadline_ = deadline_; }) {
      h.promise().deadline_ = deadline_;
    }
    // Coroutine stays in its slot - engine will find it via bitmask scan
  }

  void await_resume() const noexcept {
    // Could clear deadline in promise here if needed
  }
};

// ActivityTask: Coroutine type for HSM activities
// Activities can co_await other ActivityTasks or awaitables like SleepAwaitable.
// The HSM manages the coroutine lifetime and invokes completion callbacks.
struct ActivityTask {
  struct promise_type {
    std::coroutine_handle<> continuation_{nullptr};
    std::function<void()> on_done_callback_;
    bool started_{false};

    ActivityTask get_return_object() noexcept {
      return ActivityTask{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept {
      struct FinalAwaiter {
        std::function<void()>* callback;
        std::coroutine_handle<>* continuation;

        bool await_ready() noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept {
          if (callback && *callback) {
            (*callback)();
          }
          if (continuation && *continuation) {
            return *continuation;
          }
          return std::noop_coroutine();
        }

        void await_resume() noexcept {}
      };
      return FinalAwaiter{&on_done_callback_, &continuation_};
    }

    void return_void() noexcept {}
    void unhandled_exception() { std::terminate(); }

    // Pool-based allocation for member function coroutines
    // For member functions, args are: (this, pool, ...)
    // We embed a FrameHeader before the frame for type-erased deallocation
    template<typename Self, typename Pool, typename... Args>
      requires requires(Pool& p, std::size_t s) { { p.allocate(s) } -> std::same_as<void*>; }
    static void* operator new(std::size_t size, Self&, Pool& pool, Args&&...) {
      // Allocate extra space for header
      void* raw = pool.allocate(size + sizeof(FrameHeader));
      // Store header at the beginning
      auto* header = static_cast<FrameHeader*>(raw);
      header->deallocate_fn = &Pool::deallocate_impl;
      header->pool = &pool;
      // Return pointer after the header
      return static_cast<char*>(raw) + sizeof(FrameHeader);
    }

    // Fallback for coroutines without pool parameter (uses heap)
    static void* operator new(std::size_t size) {
      // Allocate extra space for header
      void* raw = ::operator new(size + sizeof(FrameHeader));
      // Store header indicating heap allocation
      auto* header = static_cast<FrameHeader*>(raw);
      header->deallocate_fn = nullptr;
      header->pool = nullptr;
      return static_cast<char*>(raw) + sizeof(FrameHeader);
    }

    // Single delete handles both pool and heap allocations
    static void operator delete(void* ptr) {
      if (!ptr) return;
      // Get the header stored before the frame
      auto* header = reinterpret_cast<FrameHeader*>(
        static_cast<char*>(ptr) - sizeof(FrameHeader));
      if (header->deallocate_fn) {
        // Pool allocation - use type-erased deallocate
        header->deallocate_fn(header->pool, header);
      } else {
        // Heap allocation
        ::operator delete(header);
      }
    }
  };

  std::coroutine_handle<promise_type> handle_{nullptr};

  ActivityTask() noexcept = default;
  explicit ActivityTask(std::coroutine_handle<promise_type> h) noexcept : handle_(h) {}

  ActivityTask(const ActivityTask&) = delete;
  ActivityTask& operator=(const ActivityTask&) = delete;

  ActivityTask(ActivityTask&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
  }

  ActivityTask& operator=(ActivityTask&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  ~ActivityTask() {
    if (handle_) {
      handle_.destroy();
    }
  }

  // Check if coroutine has completed
  [[nodiscard]] bool done() const noexcept {
    return !handle_ || handle_.done();
  }

  // Check if task is joinable (has work remaining)
  [[nodiscard]] bool joinable() const noexcept {
    return handle_ && !handle_.done();
  }

  // Wait for task to complete (for compatibility)
  void join() {
    while (joinable()) {
      resume();
    }
  }

  // Resume execution (returns true if more work remains)
  bool resume() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
      return !handle_.done();
    }
    return false;
  }

  // Start the coroutine (first resume)
  void start() {
    if (handle_ && !handle_.promise().started_) {
      handle_.promise().started_ = true;
      handle_.resume();
    }
  }

  // Set completion callback
  void on_done(std::function<void()> callback) {
    if (handle_) {
      handle_.promise().on_done_callback_ = std::move(callback);
    }
  }

  // Make ActivityTask awaitable so activities can co_await other activities
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

// Yield: Simple awaitable that yields control back to the scheduler
struct Yield {
  bool await_ready() const noexcept { return false; }
  void await_suspend(std::coroutine_handle<>) const noexcept {}
  void await_resume() const noexcept {}
};

// Helper to create Awaitable with duration
template <typename Rep, typename Period, typename ClockType = Clock>
Awaitable<ClockType> sleep_for(std::chrono::duration<Rep, Period> d, Signal* sig = nullptr) {
  return Awaitable<ClockType>{d, sig};
}

// Helper to create Awaitable with time point
template <typename ClockType = Clock>
Awaitable<ClockType> sleep_until(typename ClockType::time_point t, Signal* sig = nullptr) {
  return Awaitable<ClockType>{t, sig};
}


// sync_wait: Test helper to synchronously wait for a coroutine to complete
// Useful for testing coroutine-based HSMs without an async runtime.
template <typename Awaitable>
auto sync_wait(Awaitable&& awaitable) {
  struct sync_wait_task {
    struct promise_type {
      std::exception_ptr exception_;
      bool done_{false};

      sync_wait_task get_return_object() noexcept {
        return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
      }

      std::suspend_never initial_suspend() noexcept { return {}; }
      std::suspend_always final_suspend() noexcept {
        done_ = true;
        return {};
      }

      void return_void() noexcept {}
      void unhandled_exception() { exception_ = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle_;

    ~sync_wait_task() {
      if (handle_) handle_.destroy();
    }
  };

  auto task = [&]() -> sync_wait_task {
    co_await std::forward<Awaitable>(awaitable);
  }();

  // Spin until done
  while (!task.handle_.promise().done_) {
    // For ActivityTask, we may need to resume it
    if constexpr (requires { awaitable.resume(); }) {
      if (!awaitable.done()) {
        awaitable.resume();
      }
    }
    std::this_thread::yield();
  }

  if (task.handle_.promise().exception_) {
    std::rethrow_exception(task.handle_.promise().exception_);
  }
}

// sync_wait overload that returns a value
template <typename Awaitable>
  requires requires(Awaitable a) { { a.await_resume() } -> std::same_as<bool>; }
bool sync_wait_result(Awaitable&& awaitable) {
  bool result = false;

  struct sync_wait_task {
    struct promise_type {
      bool* result_ptr_;
      std::exception_ptr exception_;
      bool done_{false};

      sync_wait_task get_return_object() noexcept {
        return sync_wait_task{std::coroutine_handle<promise_type>::from_promise(*this)};
      }

      std::suspend_never initial_suspend() noexcept { return {}; }
      std::suspend_always final_suspend() noexcept {
        done_ = true;
        return {};
      }

      void return_void() noexcept {}
      void unhandled_exception() { exception_ = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle_;

    ~sync_wait_task() {
      if (handle_) handle_.destroy();
    }
  };

  auto task = [&]() -> sync_wait_task {
    result = co_await std::forward<Awaitable>(awaitable);
  }();
  task.handle_.promise().result_ptr_ = &result;

  while (!task.handle_.promise().done_) {
    if constexpr (requires { awaitable.resume(); }) {
      if (!awaitable.done()) {
        awaitable.resume();
      }
    }
    std::this_thread::yield();
  }

  if (task.handle_.promise().exception_) {
    std::rethrow_exception(task.handle_.promise().exception_);
  }

  return result;
}

// Forward declarations for constructor-time attribute override
// descriptors so that detail::is_initial_attr can specialize on
// them before their full definitions later in this header.
template <detail::fixed_string Name, typename U>
struct initial_attr;

template <detail::fixed_string Name, typename... U>
struct initial_emplace;

namespace detail {

// Detect constructor-time attribute override descriptors (hsm::initial_attr)
// so we can provide dedicated HSM constructors that consume them.

template <typename T>
struct is_initial_attr : std::false_type {};

template <detail::fixed_string Name, typename U>
struct is_initial_attr<hsm::initial_attr<Name, U> > : std::true_type {};

template <detail::fixed_string Name, typename... U>
struct is_initial_attr<hsm::initial_emplace<Name, U...> > : std::true_type {};

template <typename T>
inline constexpr bool is_initial_attr_v =
  is_initial_attr<std::decay_t<T> >::value;

template <typename T>
struct is_duration : std::false_type {};

template <typename Rep, typename Period>
struct is_duration<std::chrono::duration<Rep, Period> > : std::true_type {};

template <typename T>
inline constexpr bool is_duration_v = is_duration<T>::value;

template <typename T>
struct is_time_point : std::false_type {};

template <typename Clock, typename Duration>
struct is_time_point<std::chrono::time_point<Clock, Duration> >
  : std::true_type {};

template <typename T>
inline constexpr bool is_time_point_v = is_time_point<T>::value;

// Clock/Signal classification traits used by optional HSM sugar
// configurations. By default only hsm::Clock and hsm::Signal are
// recognized; users may specialize these for their own types.
template <typename T>
struct is_clock : std::false_type {};

template <typename T>
struct is_signal : std::false_type {};

template <typename T>
inline constexpr bool is_clock_v = is_clock<T>::value;

template <typename T>
inline constexpr bool is_signal_v = is_signal<T>::value;

template<>
struct is_clock<hsm::Clock> : std::true_type {};

template<>
struct is_signal<hsm::Signal> : std::true_type {};

// Traits for extracting event type from handler
template <typename T>
struct extract_event_type {
  using type = void;
};

template <typename Tuple>
struct get_event_from_args {
  using type = void;
};

template <typename A, typename B, typename C>
struct get_event_from_args<std::tuple<A, B, C> > {
  using type = std::decay_t<C>;
};
template <typename A, typename B>
struct get_event_from_args<std::tuple<A, B> > {
  using type = std::decay_t<B>;
};
template <typename A>
struct get_event_from_args<std::tuple<A> > {
  using type = std::decay_t<A>;
};

template <typename L>
requires requires { &L::operator(); }
struct extract_event_type<L> : extract_event_type<decltype(&L::operator())> {};

template <typename R, typename C, typename ... Args>
struct extract_event_type<R (C::*)(Args...) const> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename C, typename ... Args>
struct extract_event_type<R (C::*)(Args...) const noexcept> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename C, typename ... Args>
struct extract_event_type<R (C::*)(Args...)> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename C, typename ... Args>
struct extract_event_type<R (C::*)(Args...) noexcept> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename ... Args>
struct extract_event_type<R (*)(Args...)> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename ... Args>
struct extract_event_type<R (*)(Args...) noexcept> {
  using type = typename get_event_from_args<std::tuple<Args...> >::type;
};

// Traits for extracting instance type from handler
template <typename T>
struct extract_instance_type {
  using type = void;
};

struct not_invoked {};

// Helper for static_assert branches that depend on the handler type.
template <typename>
inline constexpr bool always_false = false;

template <typename Tuple>
struct get_instance_from_args {
  using type = void;
};

template <typename A, typename B, typename C>
struct get_instance_from_args<std::tuple<A, B, C> > {
  using type = std::decay_t<B>;
};
template <typename A, typename B>
struct get_instance_from_args<std::tuple<A, B> > {
  using type = std::decay_t<A>;
};
template <typename A>
struct get_instance_from_args<std::tuple<A> > {
  using type = std::decay_t<A>;
};

template <typename L>
requires requires { &L::operator(); }
struct extract_instance_type<L>
  : extract_instance_type<decltype(&L::operator())> {};

template <typename R, typename C, typename ... Args>
struct extract_instance_type<R (C::*)(Args...) const> {
  // For member function pointers, default to using the HSM instance
  // type (Self) rather than trying to deduce a separate instance
  // parameter from the argument list. This allows traits/mixins with
  // member behaviors (e.g., &FooBase::on_entry) to work naturally when
  // the concrete machine type derives from FooBase.
  using type = void;
};
template <typename R, typename C, typename ... Args>
struct extract_instance_type<R (C::*)(Args...) const noexcept> {
  using type = void;
};
template <typename R, typename C, typename ... Args>
struct extract_instance_type<R (C::*)(Args...)> {
  using type = void;
};
template <typename R, typename C, typename ... Args>
struct extract_instance_type<R (C::*)(Args...) noexcept> {
  using type = void;
};
template <typename R, typename ... Args>
struct extract_instance_type<R (*)(Args...)> {
  using type = typename get_instance_from_args<std::tuple<Args...> >::type;
};
template <typename R, typename ... Args>
struct extract_instance_type<R (*)(Args...) noexcept> {
  using type = typename get_instance_from_args<std::tuple<Args...> >::type;
};

// Operation signature traits used by HSM::call() to perform
// compile-time argument compatibility checks for operation callables.
//
// Primary template intentionally left without members so that
// unsupported callables trigger clean diagnostics via
// is_supported_operation / static_assert.
template <typename F>
struct operation_signature {
};

// Pointer-to-member (non-const)
template <typename R, typename C, typename... Args>
struct operation_signature<R (C::*)(Args...)> {
  using instance_type = C;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = true;
  using return_type = R;
};

// Pointer-to-member (const)
template <typename R, typename C, typename... Args>
struct operation_signature<R (C::*)(Args...) const> {
  using instance_type = C;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = true;
  using return_type = R;
};

// Pointer-to-member (noexcept)
template <typename R, typename C, typename... Args>
struct operation_signature<R (C::*)(Args...) noexcept> {
  using instance_type = C;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = true;
  using return_type = R;
};

template <typename R, typename C, typename... Args>
struct operation_signature<R (C::*)(Args...) const noexcept> {
  using instance_type = C;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = true;
  using return_type = R;
};

// Free/static function pointer
template <typename R, typename... Args>
struct operation_signature<R (*)(Args...)> {
  using instance_type = void;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = false;
  using return_type = R;
};

template <typename R, typename... Args>
struct operation_signature<R (*)(Args...) noexcept> {
  using instance_type = void;
  using args_tuple = std::tuple<Args...>;
  static constexpr bool is_member = false;
  using return_type = R;
};

// Helper trait to detect whether a callable is a supported
// operation target (i.e., we have an operation_signature
// specialization with an args_tuple alias).

template <typename F, typename = void>
struct is_supported_operation : std::false_type {
};

template <typename F>
struct is_supported_operation<F, std::void_t<typename operation_signature<F>::args_tuple> >
  : std::true_type {
};

template <typename F>
inline constexpr bool is_supported_operation_v = is_supported_operation<F>::value;

}  // namespace detail

// Polymorphic event base type. All events derive from this and provide a
// concrete kind_value() implementation so that runtime dispatch can recover
// the event kind while compile-time code continues to use E::kind.
struct EventBase {
  virtual ~EventBase() = default;
  virtual kind_t kind_value() const noexcept = 0;
};

template<hsm::kind_t K>
struct Event : EventBase {
  static constexpr hsm::kind_t kind = K;

  [[nodiscard]] kind_t kind_value() const noexcept override {
    return K;
  }
};

template<typename ... Bases>
constexpr auto event_kind(kind_t id, Bases... args) {
  // Always ensure hsm::Kind::Event is in the inheritance chain unless it already is.
  return hsm::make_kind (
    id,
    Kind::Event,
    std::forward<Bases> (args)...
  );
}

template<typename ... Bases>
constexpr auto event_kind(std::string_view name, Bases... bases) {
  return hsm::make_kind (
    detail::fnv1a_64 (name),
    Kind::Event,
    std::forward<Bases> (bases)...
  );
}

// Concrete wildcard event type used when a transition or behavior should
// react to any event kind.
struct AnyEvent : Event<static_cast<hsm::kind_t> (hsm::Kind::AnyEvent)> {};

struct InitialEvent
  : Event<static_cast<hsm::kind_t> (hsm::Kind::InitialEvent)> {};

struct CompletionEvent
  : Event<static_cast<hsm::kind_t> (hsm::Kind::CompletionEvent)> {};

struct TimeEvent : Event<static_cast<hsm::kind_t> (hsm::Kind::TimeEvent)> {};

inline constexpr std::size_t SnapshotAttributeNameCapacity = 128;

struct AttributeSnapshot {
  std::array<char, SnapshotAttributeNameCapacity> Name{};
  std::size_t NameLen{0};
  const void *Value{nullptr};

  [[nodiscard]] constexpr std::string_view name() const noexcept {
    return {Name.data(), NameLen};
  }
};

struct EventDetail {
  std::string_view Name{};
  hsm::kind_t Kind{0};
  std::string_view Target{};
  bool Guard{false};
  const void *Schema{nullptr};
};

using EventSnapshot = EventDetail;

template <std::size_t AttributeCapacity, std::size_t EventCapacity>
struct Snapshot {
  std::string_view ID{};
  std::string_view QualifiedName{};
  std::string_view State{};
  std::array<AttributeSnapshot, AttributeCapacity> Attributes{};
  std::size_t AttributeLen{0};
  std::size_t QueueLen{0};
  std::array<EventSnapshot, EventCapacity> Events{};
  std::size_t EventLen{0};
};

// Note: name-based attribute change events are represented via
// Event<k> where k is computed from the attribute name and
// Kind::ChangeEvent; we do not currently expose a dedicated
// ChangeEvent<Name> type in the public API.

namespace detail {

// Named event backed by a compile-time fixed_string and FNV-1a hash.
// This allows users to define events that originate from string literals
// but participate fully in the typed dispatch pipeline.
// Uses make_kind to match how on("name") is normalized in the model.
// Defined early so extract_events.hpp can use it.
template <auto Name>
requires fixed_string_constant<Name>
struct named_event : hsm::Event<hsm::make_kind(fnv1a_64(Name.view ()), hsm::Kind::Event)> {
  static constexpr auto name = Name;
};

} // namespace detail

} // namespace hsm

#include "detail/extract_events.hpp"

namespace hsm {

// Runtime dispatch interface shared by HSM and Group so that generic
// code can drive machines polymorphically using EventBase.
struct Instance {
  virtual ~Instance() = default;

  virtual void dispatch(const EventBase &e) = 0;
  virtual std::string_view id() const noexcept = 0;
  virtual std::string_view state() const noexcept = 0;
  // Note: start() returns Task in HSM (non-virtual, typed).
  // Polymorphic start() removed - use concrete HSM type for engine control.
};

struct unit_instance {
  unit_instance() = default;
};

// Policy type controlling the capacity of the unified internal
// event queue. QueuePolicy must provide:
//   static constexpr std::size_t capacity;
// and the capacity is interpreted as the maximum number of pending
// events (deferred or queued-during-transit) that can be stored.
//
// The default queue policy derives its capacity from the normalized
// model at compile time and uses the number of transitions as a
// conservative upper bound on useful in-flight events. To avoid
// zero-sized arrays for degenerate models, we clamp the capacity to
// at least 1.
//
// Users can override this by supplying a custom QueuePolicy type with
// a different static capacity.

template <auto Model>
struct DefaultQueuePolicy {
  private:
  using normalized_model_t = decltype(detail::normalize<Model>());

  public:
  static constexpr std::size_t raw_capacity = normalized_model_t::transition_count;
  static constexpr std::size_t capacity = raw_capacity == 0 ? 1 : raw_capacity;
};

namespace detail {

template <typename ... Ts>
[[nodiscard]] constexpr auto make_node_tuple(Ts &&... values) {
  return detail::make_structural_tuple (std::forward<Ts> (values)...);
}

// Note: named_event is defined earlier in this file (before extract_events.hpp include)
// so that it can be used for variant construction.

// Call event used by HSM::call<Name>(). This is a named event whose
// kind is derived from the operation name and hsm::Kind::CallEvent
// and which carries a tuple of arguments as its payload.
template <auto Name, typename Tuple>
requires fixed_string_constant<Name>
struct call_event
  : hsm::Event<hsm::make_kind(
      fnv1a_64(Name.view ()),
      hsm::Kind::CallEvent
    )> {
  static constexpr auto name = Name;
  using args_type = Tuple;
  Tuple args;
};

// Lightweight descriptor used by on_call("name") transitions. It
// exposes the CallEvent name via view()/size(), but does not hardcode
// the event kind; normalization derives the CallEvent-kind from the
// name so that multiple operations with different names map to distinct
// CallEvent kinds.
template <typename Name>
struct call_event_name {
  Name name;

  // Tag used by normalization to recognize this descriptor and compute
  // a CallEvent-kind from the stored name.
  using is_call_event_name = std::true_type;

  [[nodiscard]] constexpr std::string_view view() const {
    return name.view ();
  }

  [[nodiscard]] constexpr std::size_t size() const {
    return view ().size ();
  }
};

template <typename T>
struct typed_kind {
  using type = T;
  static constexpr hsm::kind_t kind = T::kind;

  [[nodiscard]] constexpr std::string_view view() const {
    if constexpr (requires { T::name.view (); }) {
      return T::name.view ();
    } else {
      return "";
    }
  }

  [[nodiscard]] constexpr std::size_t size() const {
    return view ().size ();
  }
};

template <typename Name>
struct attribute_timer_source {
  using name_type = Name;
};

template <typename T>
struct is_attribute_timer_source : std::false_type {};

template <typename Name>
struct is_attribute_timer_source<attribute_timer_source<Name> >
  : std::true_type {};

template <typename T>
inline constexpr bool is_attribute_timer_source_v =
  is_attribute_timer_source<std::remove_cvref_t<T> >::value;

}  // namespace detail

// Public alias so users can refer to the call-event type in behaviors
// and transition declarations.
template <auto Name, typename Tuple>
using CallEvent = detail::call_event<Name, Tuple>;

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto state(Name name, Partials &&... partials) {
  using name_type = std::decay_t<Name>;
  return detail::state_expr<name_type, std::decay_t<Partials>...>{
    {},
    name_type{name},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}


template <std::size_t N, typename ... Partials>
[[nodiscard]] constexpr auto state(const char (&name)[N],
                                   Partials &&... partials) {
  detail::assert_name_contains_no_slash(name, "State name cannot contain '/'");
  return state (
    detail::make_fixed_string (name),
    std::forward<Partials> (partials)...
  );
}

namespace detail {
  template <typename T> struct is_guard_expr : std::false_type {};
  template <typename C> struct is_guard_expr<guard_expr<C>> : std::true_type {};

  template <typename T> struct is_transition_expr : std::false_type {};
  template <typename... Ps> struct is_transition_expr<transition_expr<Ps...>> : std::true_type {};
  
  template <typename T> inline constexpr bool is_transition_expr_v = is_transition_expr<T>::value;

  template <typename T> struct transition_has_guard : std::false_type {};
  template <typename... Ps> struct transition_has_guard<transition_expr<Ps...>> {
      static constexpr bool value = (is_guard_expr<Ps>::value || ...);
  };
  
  template <typename T> inline constexpr bool transition_has_guard_v = transition_has_guard<T>::value;
}

// Named UML history pseudostate declarations. These must be declared inside
// a composite state. The resulting pseudostate can be targeted by its fully
// qualified path (e.g. "/Machine/Parent/hist"). When no prior history exists
// for the parent composite, an optional default behavior expressed via a
// direct target(...) + optional guard/effect is used instead of the
// composite's own initial chain.

template <detail::fixed_string_literal Name, typename ... Partials>
[[nodiscard]] constexpr auto shallow_history(Name name, Partials &&... partials) {
  using name_type = std::decay_t<Name>;
  return detail::history_expr<name_type, false, std::decay_t<Partials>...>{
    {},
    name_type{name},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}

template <std::size_t N, typename ... Partials>
[[nodiscard]] constexpr auto shallow_history(const char (&name)[N],
                                             Partials &&... partials)
  requires (sizeof...(Partials) > 0)
{
  detail::assert_name_contains_no_slash(name, "History name cannot contain '/'");
  return shallow_history(
    detail::make_fixed_string (name),
    std::forward<Partials> (partials)...
  );
}

template <detail::fixed_string_literal Name, typename ... Partials>
[[nodiscard]] constexpr auto deep_history(Name name, Partials &&... partials) {
  using name_type = std::decay_t<Name>;
  return detail::history_expr<name_type, true, std::decay_t<Partials>...>{
    {},
    name_type{name},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}

template <std::size_t N, typename ... Partials>
[[nodiscard]] constexpr auto deep_history(const char (&name)[N],
                                          Partials &&... partials)
  requires (sizeof...(Partials) > 0)
{
  detail::assert_name_contains_no_slash(name, "History name cannot contain '/'");
  return deep_history(
    detail::make_fixed_string (name),
    std::forward<Partials> (partials)...
  );
}
template <typename ... Partials>
[[nodiscard]] constexpr auto transition(Partials &&... partials) {
  return detail::transition_expr<std::decay_t<Partials>...>{
    {},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}

template <typename ... Partials>
[[nodiscard]] constexpr auto initial(Partials &&... partials) {
  return detail::initial_expr<std::decay_t<Partials>...>{
    {},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto choice(Name name, Partials &&... partials) {
  using name_type = std::decay_t<Name>;
  
  // Validation: choice must have a fallback
  static_assert(sizeof...(Partials) > 0, "Choice pseudostate must have at least one transition");
  
  using last_type = std::tuple_element_t<sizeof...(Partials) - 1, std::tuple<std::remove_cvref_t<Partials>...>>;
  
  // Helper traits for validation
  // (Defined locally or we can use existing detail namespace if we move them out, 
  // but to keep diff small we can use constexpr boolean checks if possible or rely on detail::expressions)
  
  // We need to inspect the type 'last_type' which should be a detail::transition_expr<...>
  // and ensure it does NOT contain a detail::guard_expr<...>
  
  // Checking is_transition_expr
  // We can't easily partial specialize a local struct here.
  // So we delegate to a detail helper. See below for the added helper.
  static_assert(detail::is_transition_expr_v<last_type>, 
                "The last element of a choice(...) must be a transition (the fallback)");
                
  static_assert(!detail::transition_has_guard_v<last_type>,
                "The last transition of a choice(...) must be guardless (fallback)");

  return detail::choice_expr<name_type, std::decay_t<Partials>...>{
    {},
    name_type{name},
    detail::make_node_tuple (std::forward<Partials> (partials)...)};
}

template <std::size_t N, typename ... Partials>
[[nodiscard]] constexpr auto choice(const char (&name)[N],
                                    Partials &&... partials) {
  // Validate: choice names must NOT contain '/'
  detail::assert_name_contains_no_slash(name, "Choice name cannot contain '/'");
  
  // We also validate here since this overload delegates to the one above, 
  // but the one above will catch it transitively. 
  // However, the pack expansion matches the above signature, so validation there is sufficient.
  
  return choice (
    detail::make_fixed_string (name),
    std::forward<Partials> (partials)...
  );
}

template <typename Name>
[[nodiscard]] constexpr auto final (Name name) {
  using name_type = std::decay_t<Name>;
  return detail::final_expr<name_type>{
    {},
    name_type{name}};
}

template <std::size_t N>
[[nodiscard]] constexpr auto final (const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name, "Final state name cannot contain '/'");
  return final (detail::make_fixed_string (name));
}

// Behavior DSL taking direct callables. These overloads are constrained so
// that string literals and fixed_string-based operation names are handled by
// the dedicated operation-name overloads below.

template <typename ... Actions>
[[nodiscard]] constexpr auto entry(Actions &&... actions)
  requires (((!detail::fixed_string_literal<std::decay_t<Actions> >) &&
             (!std::is_array_v<std::remove_reference_t<Actions> >)) && ...)
{
  return detail::entry_expr<std::decay_t<Actions>...>{
    {},
    detail::make_node_tuple (std::forward<Actions> (actions)...)};
}

// entry("op1", "op2", ...) → operation_action tags resolved at runtime
// to the corresponding model-level operations.

template <detail::fixed_string_literal Name,
          detail::fixed_string_literal... Names>
[[nodiscard]] constexpr auto entry(Name name, Names... names) {
  using name_type = std::decay_t<Name>;
  return detail::entry_expr<
    detail::operation_action<name_type>,
    detail::operation_action<std::decay_t<Names>>...>{
    {},
    detail::make_node_tuple(
      detail::operation_action<name_type>{name},
      detail::operation_action<std::decay_t<Names>>{names}...)};
}

template <std::size_t N, std::size_t... Ns>
[[nodiscard]] constexpr auto entry(const char (&name)[N],
                                   const char (&...names)[Ns]) {
  detail::assert_name_contains_no_slash(name,
                                        "Operation name cannot contain '/'");
  (detail::assert_name_contains_no_slash(names,
                                         "Operation name cannot contain '/'") ,
   ...);
  return entry(detail::make_fixed_string(name),
               detail::make_fixed_string(names)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto exit(Actions &&... actions)
  requires (((!detail::fixed_string_literal<std::decay_t<Actions> >) &&
             (!std::is_array_v<std::remove_reference_t<Actions> >)) && ...)
{
  return detail::exit_expr<std::decay_t<Actions>...>{
    {},
    detail::make_node_tuple (std::forward<Actions> (actions)...)};
}

// exit("op1", "op2", ...) → operation_action tags

template <detail::fixed_string_literal Name,
          detail::fixed_string_literal... Names>
[[nodiscard]] constexpr auto exit(Name name, Names... names) {
  using name_type = std::decay_t<Name>;
  return detail::exit_expr<
    detail::operation_action<name_type>,
    detail::operation_action<std::decay_t<Names>>...>{
    {},
    detail::make_node_tuple(
      detail::operation_action<name_type>{name},
      detail::operation_action<std::decay_t<Names>>{names}...)};
}

template <std::size_t N, std::size_t... Ns>
[[nodiscard]] constexpr auto exit(const char (&name)[N],
                                  const char (&...names)[Ns]) {
  detail::assert_name_contains_no_slash(name,
                                        "Operation name cannot contain '/'");
  (detail::assert_name_contains_no_slash(names,
                                         "Operation name cannot contain '/'") ,
   ...);
  return exit(detail::make_fixed_string(name),
              detail::make_fixed_string(names)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto effect(Actions &&... actions)
  requires (((!detail::fixed_string_literal<std::decay_t<Actions> >) &&
             (!std::is_array_v<std::remove_reference_t<Actions> >)) && ...)
{
  return detail::effect_expr<std::decay_t<Actions>...>{
    {},
    detail::make_node_tuple (std::forward<Actions> (actions)...)};
}

// effect("op1", "op2", ...) → operation_action tags

template <detail::fixed_string_literal Name,
          detail::fixed_string_literal... Names>
[[nodiscard]] constexpr auto effect(Name name, Names... names) {
  using name_type = std::decay_t<Name>;
  return detail::effect_expr<
    detail::operation_action<name_type>,
    detail::operation_action<std::decay_t<Names>>...>{
    {},
    detail::make_node_tuple(
      detail::operation_action<name_type>{name},
      detail::operation_action<std::decay_t<Names>>{names}...)};
}

template <std::size_t N, std::size_t... Ns>
[[nodiscard]] constexpr auto effect(const char (&name)[N],
                                    const char (&...names)[Ns]) {
  detail::assert_name_contains_no_slash(name,
                                        "Operation name cannot contain '/'");
  (detail::assert_name_contains_no_slash(names,
                                         "Operation name cannot contain '/'") ,
   ...);
  return effect(detail::make_fixed_string(name),
                detail::make_fixed_string(names)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto activity(Actions &&... actions)
  requires (((!detail::fixed_string_literal<std::decay_t<Actions> >) &&
             (!std::is_array_v<std::remove_reference_t<Actions> >)) && ...)
{
  return detail::activity_expr<std::decay_t<Actions>...>{
    {},
    detail::make_node_tuple (std::forward<Actions> (actions)...)};
}

// activity("op1", "op2", ...) → operation_action tags

template <detail::fixed_string_literal Name,
          detail::fixed_string_literal... Names>
[[nodiscard]] constexpr auto activity(Name name, Names... names) {
  using name_type = std::decay_t<Name>;
  return detail::activity_expr<
    detail::operation_action<name_type>,
    detail::operation_action<std::decay_t<Names>>...>{
    {},
    detail::make_node_tuple(
      detail::operation_action<name_type>{name},
      detail::operation_action<std::decay_t<Names>>{names}...)};
}

template <std::size_t N, std::size_t... Ns>
[[nodiscard]] constexpr auto activity(const char (&name)[N],
                                      const char (&...names)[Ns]) {
  detail::assert_name_contains_no_slash(name,
                                        "Operation name cannot contain '/'");
  (detail::assert_name_contains_no_slash(names,
                                         "Operation name cannot contain '/'") ,
   ...);
  return activity(detail::make_fixed_string(name),
                  detail::make_fixed_string(names)...);
}

template <typename Callable>
[[nodiscard]] constexpr auto guard(Callable &&callable)
  requires (!detail::fixed_string_literal<std::decay_t<Callable> > &&
            !std::is_array_v<std::remove_reference_t<Callable> >)
{
  return detail::guard_expr<std::decay_t<Callable> >{
    {},
    std::forward<Callable> (callable)};
}

// guard("name") → operation_guard tag resolved to a named operation that
// must be usable as a boolean predicate once invoked via the behavior
// invocation matrix.

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto guard(Name name) {
  using name_type = std::decay_t<Name>;
  return detail::guard_expr<detail::operation_guard<name_type>>{
    {},
    detail::operation_guard<name_type>{name}
  };
}

template <std::size_t N>
[[nodiscard]] constexpr auto guard(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name,
                                        "Operation name cannot contain '/'");
  return guard(detail::make_fixed_string(name));
}

// Model-level attribute declarations. Attributes are metadata-only
// nodes that can be addressed by name at compile time via HSM::get
// and HSM::set.

template <typename T, detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto attribute(Name name) {
  using name_type = std::decay_t<Name>;
  using value_type = T;
  // HasDefault = false: attribute_expr only stores the name; the
  // default value is represented in attribute_desc, not in the
  // model expression itself, to keep the model structural even for
  // non-structural T (e.g., std::string).
  return detail::attribute_expr<name_type, value_type, false>{
    name_type{name}
  };
}

template <typename T, std::size_t N>
[[nodiscard]] constexpr auto attribute(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name, "Attribute name cannot contain '/'");
  return attribute<T>(detail::make_fixed_string(name));
}

template <typename T, detail::fixed_string_literal Name, typename U>
[[nodiscard]] constexpr auto attribute(Name name, U &&default_value) {
  using name_type = std::decay_t<Name>;
  using value_type = T;
  return detail::attribute_expr<name_type, value_type, true>{
    name_type{name},
    static_cast<value_type>(std::forward<U> (default_value))
  };
}

// Type-deducing attribute overload: infer T from the default value.
// Example: attribute("value", 0) → T = int; attribute("flag", false) → T = bool.
template <detail::fixed_string_literal Name, typename U>
[[nodiscard]] constexpr auto attribute(Name name, U &&default_value) {
  using name_type = std::decay_t<Name>;
  using value_type = std::decay_t<U>;
  return detail::attribute_expr<name_type, value_type, true>{
    name_type{name},
    static_cast<value_type>(std::forward<U> (default_value))
  };
}

template <typename T, std::size_t N, typename U>
[[nodiscard]] constexpr auto attribute(const char (&name)[N], U &&default_value) {
  detail::assert_name_contains_no_slash(name, "Attribute name cannot contain '/'");
  return attribute<T>(detail::make_fixed_string(name), std::forward<U> (default_value));
}

// Type-deducing attribute overload for string literals and other char arrays.
// Example: attribute("name", 0) or attribute("flag", false).
template <std::size_t N, typename U>
[[nodiscard]] constexpr auto attribute(const char (&name)[N], U &&default_value) {
  detail::assert_name_contains_no_slash(name, "Attribute name cannot contain '/'");
  return attribute(detail::make_fixed_string(name), std::forward<U> (default_value));
}

// Model-level operation declarations. Operations are named callables
// that can be invoked via HSM::call<"name">(args...) and whose
// invocation first drives a corresponding CallEvent through the
// state machine.
//
// Supported callables are:
//   - pointer-to-member functions (e.g., &Self::fn)
//   - free/static function pointers (e.g., &fn)
// Other callable forms can be added later by extending
// detail::operation_signature.

template <detail::fixed_string_literal Name, typename Callable>
[[nodiscard]] constexpr auto operation(Name name, Callable callable) {
  using name_type = std::decay_t<Name>;
  using callable_type = std::decay_t<Callable>;

  static_assert(
    detail::is_supported_operation_v<callable_type>,
    "hsm::operation(Name, Callable) requires Callable to be a pointer-to-"
    "member function or function pointer"
  );

  return detail::operation_expr<name_type, callable_type>{
    name_type{name},
    callable_type{callable}
  };
}

template <std::size_t N, typename Callable>
[[nodiscard]] constexpr auto operation(const char (&name)[N], Callable callable) {
  detail::assert_name_contains_no_slash(name, "Operation name cannot contain '/'");
  return operation(detail::make_fixed_string(name), std::forward<Callable> (callable));
}

// Constructor-time attribute override descriptor and DSL. These are
// used only in HSM constructors and do NOT emit change events.
// Example:
//   using M = hsm::HSM<model, M>;
//   M sm(hsm::set<"value">(123), hsm::set<"flag">(true));
//
// This overrides the initial attribute values without driving
// when("name") transitions.
template <detail::fixed_string Name, typename U>
struct initial_attr {
  using name_type  = decltype(Name);
  using value_type = std::decay_t<U>;
  value_type value;
};

template <detail::fixed_string Name, typename U>
[[nodiscard]] constexpr auto set(U &&v) {
  using value_type = std::decay_t<U>;
  return initial_attr<Name, value_type>{
    static_cast<value_type>(std::forward<U> (v))
  };
}

// Emplace-style constructor-time descriptor to build attributes in-place
// from constructor arguments without emitting change events. This is
// particularly useful for complex types (e.g. std::string, std::vector).
// Example:
//   M sm(hsm::emplace<"name">("hello"),
//        hsm::emplace<"vec">(3, 7));
// which will behave as if:
//   get<"name">() == std::string("hello");
//   get<"vec">()  == std::vector<int>(3, 7);
template <detail::fixed_string Name, typename... U>
struct initial_emplace {
  using name_type  = decltype(Name);
  using tuple_type = std::tuple<U...>;
  tuple_type args;
};

template <detail::fixed_string Name, typename... Args>
[[nodiscard]] constexpr auto emplace(Args &&... args) {
  using tuple_type = std::tuple<std::decay_t<Args>...>;
  return initial_emplace<Name, std::decay_t<Args>...>{
    tuple_type{std::forward<Args> (args)...}
  };
}
template <typename T>
[[nodiscard]] constexpr auto on() {
  return detail::on_expr<detail::typed_kind<T> >{
    {},
    detail::typed_kind<T>{}};
}

// String-literal and fixed_string based events
// These remain hashed string events but integrate with the
// normalization pipeline via fixed_string.
template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto on(Name name) {
  return detail::on_expr<Name>{
    {},
    name};
}

template <std::size_t N>
[[nodiscard]] constexpr auto on(const char (&name)[N]) {
  return on (detail::make_fixed_string (name));
}

// Call-event DSL: on_call("name") declares a transition that listens
// for CallEvent-kind events associated with the named operation.
// The name is hashed and combined with Kind::CallEvent so that
// HSM::call<Name>() dispatches to these transitions.

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto on_call(Name name) {
  using name_type = std::decay_t<Name>;
  using event_type = detail::call_event_name<name_type>;
  return detail::on_expr<event_type>{
    {},
    event_type{ name_type{name} }
  };
}

template <std::size_t N>
[[nodiscard]] constexpr auto on_call(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name, "Operation name cannot contain '/'");
  return on_call(detail::make_fixed_string(name));
}

template <typename Path>
[[nodiscard]] constexpr auto source(Path path) {
  using path_type = std::decay_t<Path>;
  return detail::source_expr<path_type>{path_type{path}};
}

template <std::size_t N>
[[nodiscard]] constexpr auto source(const char (&path)[N]) {
  // Validate: path must be absolute (start with /)
  
  return source(detail::make_fixed_string(path));
}
template <typename Path>
[[nodiscard]] constexpr auto target(Path path) {
  using path_type = std::decay_t<Path>;
  return detail::target_expr<path_type>{path_type{path}};
}

template <std::size_t N>
[[nodiscard]] constexpr auto target(const char (&path)[N]) {
  detail::assert_path_absolute(path, "Target path must be absolute and start with '/'");
  return target(detail::make_fixed_string(path));
}
// UML 2.5 history pseudostate *targets* (legacy API)
// Usage:
//   transition(on(Event{}), target(hsm::shallow_history("/Parent")))
//   transition(on(Event{}), target(hsm::deep_history("/Parent")))
// where "/Parent" is the absolute path of the composite state whose
// history is being targeted.
//
// Newer models should prefer named history pseudostates declared via
// shallow_history("hist", target("/default"), ...) / deep_history(...)
// and target them by path (e.g. "/Machine/Parent/hist").

template <typename Path>
[[nodiscard]] constexpr auto shallow_history(Path parent) {
  using path_type = std::decay_t<Path>;
  return detail::shallow_history_path<path_type>{path_type{parent}};
}
template <std::size_t N>
[[nodiscard]] constexpr auto shallow_history(const char (&parent)[N]) {
  return shallow_history (detail::make_fixed_string (parent));
}

template <typename Path>
[[nodiscard]] constexpr auto deep_history(Path parent) {
  using path_type = std::decay_t<Path>;
  return detail::deep_history_path<path_type>{path_type{parent}};
}

template <std::size_t N>
[[nodiscard]] constexpr auto deep_history(const char (&parent)[N]) {
  return deep_history (detail::make_fixed_string (parent));
}

template <typename ... Events>
[[nodiscard]] constexpr auto defer() {
  // Use a tuple of typed_kind for types
  return detail::defer_expr<detail::typed_kind<std::decay_t<Events> >...>{
    detail::make_structural_tuple (detail::typed_kind<std::decay_t<Events> >{} ...)};
}



template <typename Callable>
[[nodiscard]] constexpr auto after(Callable &&callable)
  requires (!detail::fixed_string_literal<std::decay_t<Callable> > &&
            !std::is_array_v<std::remove_reference_t<Callable> >)
{
  return detail::after_expr<std::decay_t<Callable> >{
    std::forward<Callable> (callable)};
}

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto after(Name name) {
  using name_type = std::decay_t<Name>;
  static_cast<void>(name);
  return detail::after_expr<detail::attribute_timer_source<name_type> >{
    detail::attribute_timer_source<name_type>{}
  };
}

template <std::size_t N>
[[nodiscard]] constexpr auto after(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name,
                                        "Timer attribute name cannot contain '/'");
  return after(detail::make_fixed_string(name));
}


template <typename Callable>
[[nodiscard]] constexpr auto every(Callable &&callable)
  requires (!detail::fixed_string_literal<std::decay_t<Callable> > &&
            !std::is_array_v<std::remove_reference_t<Callable> >)
{
  return detail::every_expr<std::decay_t<Callable> >{
    std::forward<Callable> (callable)};
}

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto every(Name name) {
  using name_type = std::decay_t<Name>;
  static_cast<void>(name);
  return detail::every_expr<detail::attribute_timer_source<name_type> >{
    detail::attribute_timer_source<name_type>{}
  };
}

template <std::size_t N>
[[nodiscard]] constexpr auto every(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name,
                                        "Timer attribute name cannot contain '/'");
  return every(detail::make_fixed_string(name));
}

// Non-UML aliases for timer-based triggers to aid users unfamiliar
// with UML terminology.
//   on_timeout(source)   -> after(source)
//   on_interval(source)  -> every(source)
//   on_timepoint(source) -> at(source) (defined further below)

template <typename Source>
[[nodiscard]] constexpr auto on_timeout(Source &&source) {
  return after(std::forward<Source> (source));
}

template <typename Source>
[[nodiscard]] constexpr auto on_interval(Source &&source) {
  return every(std::forward<Source> (source));
}

// Attribute-based when(): DSL node keyed by attribute name.
//
// Example:
//   constexpr auto model = define(
//       "machine",
//       attribute<int>("value", 0),
//       initial(target("/machine/idle")),
//       state("idle",
//             transition(when("value"),
//                        target("/machine/done"))));
//
//   struct Machine : hsm::HSM<model, Machine> {};
//   Machine sm;
//   sm.set<"value">(42); // drives the when("value") transition.
//
// This form replaces both the legacy polling-based when(predicate)
// timer and the member-pointer-based ChangeEvent sugar.
template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto when(Name name) {
  using name_type = std::decay_t<Name>;
  return detail::when_attr_expr<name_type>{name_type{name}};
}

template <std::size_t N>
[[nodiscard]] constexpr auto when(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name, "Attribute name cannot contain '/'");
  return when(detail::make_fixed_string(name));
}

// Non-UML alias for attribute-change trigger:
//   on_set("attr") -> when("attr")

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto on_set(Name name) {
  return when(name);
}

template <std::size_t N>
[[nodiscard]] constexpr auto on_set(const char (&name)[N]) {
  return when(name);
}


template <typename Callable>
[[nodiscard]] constexpr auto at(Callable &&callable)
  requires (!detail::fixed_string_literal<std::decay_t<Callable> > &&
            !std::is_array_v<std::remove_reference_t<Callable> >)
{
  return detail::at_expr<std::decay_t<Callable> >{
    std::forward<Callable> (callable)};
}

template <detail::fixed_string_literal Name>
[[nodiscard]] constexpr auto at(Name name) {
  using name_type = std::decay_t<Name>;
  static_cast<void>(name);
  return detail::at_expr<detail::attribute_timer_source<name_type> >{
    detail::attribute_timer_source<name_type>{}
  };
}

template <std::size_t N>
[[nodiscard]] constexpr auto at(const char (&name)[N]) {
  detail::assert_name_contains_no_slash(name,
                                        "Timer attribute name cannot contain '/'");
  return at(detail::make_fixed_string(name));
}

// Alias for at() using non-UML naming.
//   on_timepoint(source) -> at(source)

template <typename Source>
[[nodiscard]] constexpr auto on_timepoint(Source &&source) {
  return at(std::forward<Source> (source));
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto define(Name name,
                                    Partials &&... partials) noexcept {
  using namespace detail;  // Expose detail to access structural tuples
  using name_type = std::decay_t<Name>;
  using expression_type =
    detail::model_expression<name_type, std::decay_t<Partials>...>;
  return expression_type{
    name_type{name},
    detail::make_structural_tuple (std::forward<Partials> (partials)...)};
}

template <std::size_t N, typename ... Partials>
[[nodiscard]] constexpr auto define(const char (&name)[N],
                                    Partials &&... partials) noexcept {
  // Validate: model names must NOT contain '/'
  detail::assert_name_contains_no_slash(name, "Model name cannot contain '/'");
  return define (
    detail::make_fixed_string (name),
    std::forward<Partials> (partials)...
  );
}

// Canonical PascalCase DSL surface. Lowercase functions remain supported as
// C++-native aliases, but dsl.md defines these names as the portable contract.
template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto Define(Name &&name, Partials &&... partials)
  noexcept(noexcept(define(std::forward<Name>(name),
                           std::forward<Partials>(partials)...))) {
  return define(std::forward<Name>(name), std::forward<Partials>(partials)...);
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto State(Name &&name, Partials &&... partials)
  noexcept(noexcept(state(std::forward<Name>(name),
                          std::forward<Partials>(partials)...))) {
  return state(std::forward<Name>(name), std::forward<Partials>(partials)...);
}

template <typename Name>
[[nodiscard]] constexpr auto Final(Name &&name)
  noexcept(noexcept(final(std::forward<Name>(name)))) {
  return final(std::forward<Name>(name));
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto ShallowHistory(Name &&name, Partials &&... partials)
  noexcept(noexcept(shallow_history(std::forward<Name>(name),
                                    std::forward<Partials>(partials)...))) {
  return shallow_history(std::forward<Name>(name),
                         std::forward<Partials>(partials)...);
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto DeepHistory(Name &&name, Partials &&... partials)
  noexcept(noexcept(deep_history(std::forward<Name>(name),
                                 std::forward<Partials>(partials)...))) {
  return deep_history(std::forward<Name>(name),
                      std::forward<Partials>(partials)...);
}

template <typename Name, typename ... Partials>
[[nodiscard]] constexpr auto Choice(Name &&name, Partials &&... partials)
  noexcept(noexcept(choice(std::forward<Name>(name),
                           std::forward<Partials>(partials)...))) {
  return choice(std::forward<Name>(name), std::forward<Partials>(partials)...);
}

template <typename ... Partials>
[[nodiscard]] constexpr auto Transition(Partials &&... partials)
  noexcept(noexcept(transition(std::forward<Partials>(partials)...))) {
  return transition(std::forward<Partials>(partials)...);
}

template <typename ... Partials>
[[nodiscard]] constexpr auto Initial(Partials &&... partials)
  noexcept(noexcept(initial(std::forward<Partials>(partials)...))) {
  return initial(std::forward<Partials>(partials)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto Entry(Actions &&... actions)
  noexcept(noexcept(entry(std::forward<Actions>(actions)...))) {
  return entry(std::forward<Actions>(actions)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto Exit(Actions &&... actions)
  noexcept(noexcept(exit(std::forward<Actions>(actions)...))) {
  return exit(std::forward<Actions>(actions)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto Activity(Actions &&... actions)
  noexcept(noexcept(activity(std::forward<Actions>(actions)...))) {
  return activity(std::forward<Actions>(actions)...);
}

template <typename ... Actions>
[[nodiscard]] constexpr auto Effect(Actions &&... actions)
  noexcept(noexcept(effect(std::forward<Actions>(actions)...))) {
  return effect(std::forward<Actions>(actions)...);
}

template <typename Callable>
[[nodiscard]] constexpr auto Guard(Callable &&callable)
  noexcept(noexcept(guard(std::forward<Callable>(callable)))) {
  return guard(std::forward<Callable>(callable));
}

template <typename T>
[[nodiscard]] constexpr auto On()
  noexcept(noexcept(on<T>())) {
  return on<T>();
}

template <typename Name>
[[nodiscard]] constexpr auto On(Name &&name)
  noexcept(noexcept(on(std::forward<Name>(name)))) {
  return on(std::forward<Name>(name));
}

template <typename Name>
[[nodiscard]] constexpr auto OnCall(Name &&name)
  noexcept(noexcept(on_call(std::forward<Name>(name)))) {
  return on_call(std::forward<Name>(name));
}

template <typename Name>
[[nodiscard]] constexpr auto When(Name &&name)
  noexcept(noexcept(when(std::forward<Name>(name)))) {
  return when(std::forward<Name>(name));
}

template <typename Name>
[[nodiscard]] constexpr auto OnSet(Name &&name)
  noexcept(noexcept(on_set(std::forward<Name>(name)))) {
  return on_set(std::forward<Name>(name));
}

template <typename Source>
[[nodiscard]] constexpr auto After(Source &&source)
  noexcept(noexcept(after(std::forward<Source>(source)))) {
  return after(std::forward<Source>(source));
}

template <typename Source>
[[nodiscard]] constexpr auto Every(Source &&source)
  noexcept(noexcept(every(std::forward<Source>(source)))) {
  return every(std::forward<Source>(source));
}

template <typename Source>
[[nodiscard]] constexpr auto At(Source &&source)
  noexcept(noexcept(at(std::forward<Source>(source)))) {
  return at(std::forward<Source>(source));
}

template <typename Path>
[[nodiscard]] constexpr auto Source(Path &&path)
  noexcept(noexcept(source(std::forward<Path>(path)))) {
  return source(std::forward<Path>(path));
}

template <typename Path>
[[nodiscard]] constexpr auto Target(Path &&path)
  noexcept(noexcept(target(std::forward<Path>(path)))) {
  return target(std::forward<Path>(path));
}

template <typename ... Events>
[[nodiscard]] constexpr auto Defer()
  noexcept(noexcept(defer<Events...>())) {
  return defer<Events...>();
}

template <typename T, typename Name>
[[nodiscard]] constexpr auto Attribute(Name &&name)
  noexcept(noexcept(attribute<T>(std::forward<Name>(name)))) {
  return attribute<T>(std::forward<Name>(name));
}

template <typename T, typename Name, typename U>
[[nodiscard]] constexpr auto Attribute(Name &&name, U &&default_value)
  noexcept(noexcept(attribute<T>(std::forward<Name>(name),
                                 std::forward<U>(default_value)))) {
  return attribute<T>(std::forward<Name>(name),
                      std::forward<U>(default_value));
}

template <typename Name, typename U>
[[nodiscard]] constexpr auto Attribute(Name &&name, U &&default_value)
  noexcept(noexcept(attribute(std::forward<Name>(name),
                              std::forward<U>(default_value)))) {
  return attribute(std::forward<Name>(name), std::forward<U>(default_value));
}

template <typename Name, typename Callable>
[[nodiscard]] constexpr auto Operation(Name &&name, Callable &&callable)
  noexcept(noexcept(operation(std::forward<Name>(name),
                              std::forward<Callable>(callable)))) {
  return operation(std::forward<Name>(name), std::forward<Callable>(callable));
}

template <auto Model,
          typename Self = unit_instance,
          typename ClockT = hsm::Clock,
          typename QueuePolicy = hsm::DefaultQueuePolicy<Model>,
          typename TaskT = hsm::Task<ClockT>,
          typename AwaitableT = hsm::Awaitable<ClockT>,
          typename SignalT = hsm::Signal,
          std::size_t FrameSize = 256,
          template<std::size_t, std::size_t> typename PromisePoolT = hsm::PromisePool>
struct HSM : Instance {
  static constexpr auto model_ = Model;
  using instance_type = Self;
  using queue_policy_type = QueuePolicy;
  using ClockType = ClockT;
  using TaskType = TaskT;
  using AwaitableType = AwaitableT;
  using SignalType = SignalT;
  static constexpr std::size_t frame_size = FrameSize;

  // Note: CRTP usage is enforced structurally by self(): it performs
  // static_cast<instance_type &>(*this), which is only well-formed when
  // instance_type is actually derived from this HSM specialization.
  // Misuse (e.g., HSM<model, PlainType>) therefore results in a hard
  // compile-time error without needing an additional static_assert here.

  // 1. Model Normalization
  static constexpr auto normalized_model = detail::normalize<model_> ();
  static constexpr std::size_t max_depth =
    decltype(normalized_model)::max_depth;

  // Detect if any deferred events exist in the model so we can
  // cheaply bypass deferral checks when unused.
  static constexpr std::size_t total_deferred_count =
    decltype(normalized_model)::deferred_count;
  static constexpr bool has_deferred_events = (total_deferred_count > 0);

  // Unified event queue capacity (for both deferred and queued-during-
  // transit events). This is policy-controlled at compile time.
  static constexpr std::size_t queue_capacity = QueuePolicy::capacity;
  static_assert(queue_capacity > 0,
                "hsm::HSM QueuePolicy::capacity must be greater than zero");

  // Storage size for the ring buffer. We keep this separate to allow
  // future experimentation with sentinel slots while preserving a
  // clear logical capacity.
  static constexpr std::size_t queue_storage_size = queue_capacity;

  // Compile-time event index lookup
  //
  // For a concrete event kind K we first look for an exact match in the
  // model's event table. If none is found but the model declares a wildcard
  // (AnyEvent) handler, we treat that wildcard entry as the effective event
  // index for K so that wildcard transitions can handle arbitrary event
  // types without requiring every possible kind to be enumerated.
  //
  // If neither a concrete match nor a wildcard entry exists, the kind is
  // considered unsupported by this model and event_index returns
  // detail::invalid_index; typed dispatch will then fail at compile time.
  template <hsm::kind_t K>
  static consteval std::size_t event_index() {
    // 1. Exact match
    for (std::size_t i = 0; i < normalized_model.event_count; ++i) {
      if (normalized_model.events[i].kind == K) {
        return i;
      }
    }
    // 2. Fallback to AnyEvent wildcard, if present
    for (std::size_t i = 0; i < normalized_model.event_count; ++i) {
      if (normalized_model.events[i].kind ==
          static_cast<hsm::kind_t>(hsm::Kind::AnyEvent)) {
        return i;
      }
    }
    // 3. No support for this kind anywhere in the model
    return detail::invalid_index;
  }

  // 2. Behavior & Attribute Extraction
  static constexpr auto entry_tuple = detail::extract_entries (model_);
  static constexpr auto exit_tuple = detail::extract_exits (model_);
  static constexpr auto activity_tuple = detail::extract_activities (model_);
  static constexpr auto guard_tuple = detail::extract_guards (model_);
  static constexpr auto effect_tuple = detail::extract_effects (model_);
  static constexpr auto timer_tuple = detail::extract_timers (model_);
  static constexpr auto attribute_tuple = detail::extract_attributes (model_);
  static constexpr auto operation_tuple = detail::extract_operations (model_);

  static constexpr std::size_t attribute_count =
    std::tuple_size_v<std::remove_cvref_t<decltype(attribute_tuple)>>;
  using snapshot_type = Snapshot<attribute_count, queue_capacity * 2U>;

  static constexpr std::size_t operation_count =
    std::tuple_size_v<std::remove_cvref_t<decltype(operation_tuple)>>;

  template <detail::fixed_string Name, std::size_t I = 0>
  static consteval std::size_t attribute_index_impl() {
    if constexpr (I >= attribute_count) {
      return detail::invalid_index;
    } else {
      constexpr auto desc = std::get<I>(attribute_tuple);
      if constexpr (desc.name.view() == Name.view()) {
        return I;
      } else {
        return attribute_index_impl<Name, I + 1>();
      }
    }
  }

  template <detail::fixed_string Name>
  static consteval std::size_t attribute_index() {
    return attribute_index_impl<Name>();
  }

  template <typename NameType, std::size_t I = 0>
  static consteval std::size_t attribute_index_from_name_type_impl() {
    if constexpr (I >= attribute_count) {
      return detail::invalid_index;
    } else {
      using desc_type =
        std::tuple_element_t<I, std::remove_cvref_t<decltype(attribute_tuple)>>;
      if constexpr (std::is_same_v<typename desc_type::name_type, NameType>) {
        return I;
      } else {
        return attribute_index_from_name_type_impl<NameType, I + 1>();
      }
    }
  }

  template <typename NameType>
  static consteval std::size_t attribute_index_from_name_type() {
    return attribute_index_from_name_type_impl<NameType>();
  }

  template <detail::fixed_string Name, std::size_t I = 0>
  static consteval std::size_t operation_index_impl() {
    if constexpr (I >= operation_count) {
      return detail::invalid_index;
    } else {
      constexpr auto desc = std::get<I>(operation_tuple);
      if constexpr (desc.name.view() == Name.view()) {
        return I;
      } else {
        return operation_index_impl<Name, I + 1>();
      }
    }
  }

  template <detail::fixed_string Name>
  static consteval std::size_t operation_index() {
    return operation_index_impl<Name>();
  }

  // Value-based operation index lookup used by operation-name behaviors.
  template <typename Name, std::size_t I = 0>
  static constexpr std::size_t operation_index_from_value_impl(const Name &name) {
    if constexpr (I >= operation_count) {
      return detail::invalid_index;
    } else {
      const auto &desc = std::get<I>(operation_tuple);
      if (desc.name.view() == name.view()) {
        return I;
      } else {
        return operation_index_from_value_impl<Name, I + 1>(name);
      }
    }
  }

  template <typename Name>
  static constexpr std::size_t operation_index_from_value(const Name &name) {
    return operation_index_from_value_impl<Name>(name);
  }

  // Name-based event type mapping: Machine::events<"name">::type
  // Resolves a compile-time name to a canonical event type according to
  // the following precedence:
  //   1) Operation name  -> CallEvent-kind event for that operation.
  //   2) Attribute name  -> ChangeEvent-kind event for that attribute.
  //   3) Otherwise       -> regular Kind::Event named event.
  template <detail::fixed_string Name>
  struct events {
  private:
    static constexpr std::size_t attr_idx = attribute_index<Name>();
    static constexpr std::size_t op_idx = operation_index<Name>();
    static constexpr bool has_attr = (attr_idx != detail::invalid_index);
    static constexpr bool has_op = (op_idx != detail::invalid_index);

    template <detail::fixed_string N,
              std::size_t OpIdx,
              std::size_t AttrIdx,
              bool HasOp,
              bool HasAttr>
    struct resolver;

    // Operation-only: map to the CallEvent associated with the operation.
    template <detail::fixed_string N, std::size_t OpIdx, std::size_t AttrIdx>
    struct resolver<N, OpIdx, AttrIdx, true, false> {
      using ops_tuple_type = std::remove_cvref_t<decltype(operation_tuple)>;
      static_assert(OpIdx < std::tuple_size_v<ops_tuple_type>,
                    "hsm::HSM::events<Name>: operation index out of range");
      using OpDesc = std::tuple_element_t<OpIdx, ops_tuple_type>;
      using Callable = typename OpDesc::callable_type;
      using Sig = detail::operation_signature<Callable>;
      using ParamTuple = typename Sig::args_tuple;

      using type = hsm::CallEvent<N, ParamTuple>;

      static constexpr bool is_operation = true;
      static constexpr bool is_attribute = false;
      static constexpr bool is_plain_event = false;

      static consteval bool supported() {
        return HSM::template supports_event<type>();
      }
    };

    // Attribute-only: ChangeEvent-kind event keyed by the attribute name.
    template <detail::fixed_string N, std::size_t OpIdx, std::size_t AttrIdx>
    struct resolver<N, OpIdx, AttrIdx, false, true> {
      static constexpr auto hash = detail::fnv1a_64(N.view());
      static constexpr auto kind =
        hsm::make_kind(hash, hsm::Kind::ChangeEvent);

      using type = hsm::Event<kind>;

      static constexpr bool is_operation = false;
      static constexpr bool is_attribute = true;
      static constexpr bool is_plain_event = false;

      static consteval bool supported() {
        return HSM::template supports_event<type>();
      }
    };

    // Neither operation nor attribute: regular named Kind::Event.
    template <detail::fixed_string N, std::size_t OpIdx, std::size_t AttrIdx>
    struct resolver<N, OpIdx, AttrIdx, false, false> {
      static constexpr auto hash = detail::fnv1a_64(N.view());
      static constexpr auto kind =
        hsm::make_kind(hash, hsm::Kind::Event);

      using type = hsm::Event<kind>;

      static constexpr bool is_operation = false;
      static constexpr bool is_attribute = false;
      static constexpr bool is_plain_event = true;

      static consteval bool supported() {
        return HSM::template supports_event<type>();
      }
    };

    // Ambiguous name: both an operation and an attribute share this name.
    template <detail::fixed_string N, std::size_t OpIdx, std::size_t AttrIdx>
    struct resolver<N, OpIdx, AttrIdx, true, true> {
      static_assert(
        OpIdx == detail::invalid_index && AttrIdx == detail::invalid_index,
        "hsm::HSM::events<Name>: Name matches both an attribute and an "
        "operation; please disambiguate"
      );

      using type = void;
      static constexpr bool is_operation = false;
      static constexpr bool is_attribute = false;
      static constexpr bool is_plain_event = false;

      static consteval bool supported() { return false; }
    };

    using impl = resolver<Name, op_idx, attr_idx, has_op, has_attr>;

  public:
    using type = typename impl::type;

    static constexpr bool is_operation = impl::is_operation;
    static constexpr bool is_attribute = impl::is_attribute;
    static constexpr bool is_plain_event = impl::is_plain_event;

    static consteval bool supported() {
      return impl::supported();
    }
  };

  // Extract all event types for variant construction
  // Uses NTTP-based extraction to preserve string event values at compile time
  static constexpr auto event_types_tuple = detail::extract_all_events_nttp<model_> ();

  using PlaceholderEvent = hsm::Event<0>;

  template <typename Tuple>
  struct to_variant;
  template <typename ... Ts>
  struct to_variant<std::tuple<Ts...> > {
    using type = std::conditional_t<(sizeof...(Ts) > 0), std::variant<Ts...>, std::variant<PlaceholderEvent> >;
  };

  using EventVariant = typename to_variant<std::remove_cvref_t<decltype(event_types_tuple)> >::type;

  // Queue entry: holds event for processing
  struct QueueEntry {
    EventVariant event;
    std::size_t event_id{0};  // Store EventId directly for O(1) dispatch
    bool is_fallback{false};  // True if typed event was substituted as AnyEvent
  };

  struct Queue {
    std::array<QueueEntry, queue_storage_size> entries{};
    std::atomic<std::size_t> head{0};
    std::atomic<std::size_t> tail{0};

    void Clear() noexcept {
      head.store(0, std::memory_order_seq_cst);
      tail.store(0, std::memory_order_seq_cst);
    }

    [[nodiscard]] std::size_t Len() const noexcept {
      const auto current_head = head.load(std::memory_order_seq_cst);
      const auto current_tail = tail.load(std::memory_order_seq_cst);
      return current_tail - current_head;
    }

    [[nodiscard]] bool Full() const noexcept {
      return Len() >= queue_capacity;
    }

    bool Push(const QueueEntry &entry) noexcept {
      if (Full()) {
        return false;
      }

      const auto current_tail = tail.load(std::memory_order_seq_cst);
      entries[current_tail % queue_capacity] = entry;
      tail.store(current_tail + 1, std::memory_order_seq_cst);
      return true;
    }

    bool Pop(QueueEntry &entry, std::size_t limit) noexcept {
      const auto current_head = head.load(std::memory_order_seq_cst);
      if (current_head >= limit) {
        return false;
      }

      entry = entries[current_head % queue_capacity];
      return true;
    }

    void CommitPop() noexcept {
      const auto current_head = head.load(std::memory_order_seq_cst);
      head.store(current_head + 1, std::memory_order_seq_cst);
    }
  };

  // Helper trait: can the unified queue store an event of type E at
  // the given EventId index? This is used to avoid instantiating
  // std::variant::emplace with incompatible argument types, which
  // would otherwise cause hard compile-time errors for models that
  // rely on AnyEvent-style wildcard handlers.
  template <std::size_t EventId, typename E, bool InRange>
  struct can_enqueue_event_helper {
    static constexpr bool value = false;
  };

  template <std::size_t EventId, typename E>
  struct can_enqueue_event_helper<EventId, E, true> {
    using Alt = std::variant_alternative_t<EventId, EventVariant>;
    static constexpr bool value = std::is_constructible_v<Alt, const E &>;
  };

  template <std::size_t EventId, typename E>
  static constexpr bool can_enqueue_event_v =
      can_enqueue_event_helper<EventId, E,
                                (EventId < std::variant_size_v<EventVariant>)>::value;

  // Helper trait: is the variant alternative at EventId the AnyEvent type?
  // This is used for wildcard dispatch where we can't store the concrete
  // event type but can substitute AnyEvent{} instead.
  template <std::size_t EventId, bool InRange>
  struct is_any_event_slot_helper {
    static constexpr bool value = false;
  };

  template <std::size_t EventId>
  struct is_any_event_slot_helper<EventId, true> {
    using Alt = std::variant_alternative_t<EventId, EventVariant>;
    static constexpr bool value = std::is_same_v<Alt, AnyEvent>;
  };

  template <std::size_t EventId>
  static constexpr bool is_any_event_slot_v =
      is_any_event_slot_helper<EventId, (EventId < std::variant_size_v<EventVariant>)>::value;

  // 3. Activity Tracking Definitions
  static constexpr std::size_t total_activity_count =
    std::tuple_size_v<decltype(activity_tuple)>;
  static constexpr std::size_t total_timer_count =
    std::tuple_size_v<decltype(timer_tuple)>;

  // Total coroutine count for pool sizing: engine + timers + activities
  // During transitions, a new coroutine may be created before the old one is
  // destroyed (e.g., self-transitions). Double the timer/activity slots for overlap.
  static constexpr std::size_t total_coro_count =
    1 + 2 * (total_timer_count + total_activity_count);

  // Pool type for coroutine frame allocation (deferred init via std::optional)
  using PoolType = PromisePoolT<frame_size, total_coro_count>;

  // Compile-time helper: count timers whose source state matches StateId.
  template <std::size_t StateId>
  static consteval std::size_t timer_count_for_state() {
    std::size_t count = 0;
    for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
      const auto &trans = normalized_model.transitions[t];
      if (trans.source_id == StateId &&
          trans.timer_type != detail::timer_kind::none) {
        ++count;
      }
    }
    return count;
  }

  // Compile-time helper: number of concurrent tasks (activities + timers)
  // along the root→StateId path in the normalized hierarchy.
  template <std::size_t StateId>
  static consteval std::size_t concurrent_tasks_on_path() {
    const auto &s = normalized_model.states[StateId];
    const std::size_t self_activities = s.activity_count;
    const std::size_t self_timers = timer_count_for_state<StateId>();
    const std::size_t self_total = self_activities + self_timers;

    if constexpr (normalized_model.states[StateId].parent_id == detail::invalid_index) {
      return self_total;
    } else {
      return self_total +
             concurrent_tasks_on_path<normalized_model.states[StateId].parent_id>();
    }
  }

  // Maximum possible number of concurrently active tasks (activities + timers)
  // across all reachable configurations for this model.
  static constexpr std::size_t max_concurrent_tasks = []() {
    std::size_t max = 0;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (([&] {
          constexpr std::size_t current = concurrent_tasks_on_path<Is>();
          if (current > max) {
            max = current;
          }
        }()), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return max;
  }();

  struct timer_event_map_t {
    static consteval auto build() {
      std::array<std::size_t, total_timer_count> timer_to_event{};
      timer_to_event.fill (detail::invalid_index);
      for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
        const auto &trans = normalized_model.transitions[t];
        if (trans.timer_type != detail::timer_kind::none &&
            trans.timer_idx != detail::invalid_index &&
            trans.timer_idx < total_timer_count &&
            trans.event_id != detail::invalid_index) {
          timer_to_event[trans.timer_idx] = trans.event_id;
        }
      }
      return timer_to_event;
    }

    static constexpr auto map = build ();

    template <hsm::kind_t K, std::size_t I>
    static consteval std::size_t kind_count_impl() {
      if constexpr (I >= normalized_model.event_count) {
        return 0;
      } else {
        return (normalized_model.events[I].kind == K ? 1u : 0u) +
               kind_count_impl<K, I + 1>();
      }
    }

    template <std::size_t TimerIdx>
    static consteval std::size_t event_id() {
      static_assert(TimerIdx < total_timer_count,
                    "hsm::HSM timer_event_map_t::event_id() TimerIdx out of range");

      constexpr std::size_t id = map[TimerIdx];
      static_assert(
        id != detail::invalid_index,
        "hsm::HSM timer_event_map_t::event_id() found no event for this TimerIdx; timer has no associated event_id"
      );

      // Ensure that the timer event's kind is unique in the model. If another
      // event shares the same kind, dispatch based on (StateId, EventId)
      // could become ambiguous when using kind-based matching.
      constexpr auto k = normalized_model.events[id].kind;
      constexpr std::size_t count = kind_count_impl<k, 0>();
      static_assert(
        count == 1,
        "hsm::HSM timer_event_map_t::event_id() detected a non-unique timer event kind; another event shares this kind"
      );

      return id;
    }

    // Eagerly validate all timer indices at compile time so that any
    // inconsistency between normalized_model timers and events is detected
    // as a hard compile-time error.
    static constexpr auto validate_all = []() {
      []<std::size_t... Is>(std::index_sequence<Is...>) {
        (static_cast<void>(event_id<Is>()), ...);
      }(std::make_index_sequence<total_timer_count>{});
      return 0;
    }();
  };

  // History Usage Map
  // Maps StateId -> Dense History Index (0..M)
  struct history_map_t {
    static consteval auto build() {
      std::array<std::size_t, normalized_model.state_count> state_to_history{};
      state_to_history.fill (detail::invalid_index);
      std::size_t history_count = 0;

      for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
        const auto &trans = normalized_model.transitions[t];
        if (trans.history != detail::history_kind::none &&
            trans.history_parent != detail::invalid_index) {
          if (state_to_history[trans.history_parent] == detail::invalid_index) {
            state_to_history[trans.history_parent] = history_count++;
          }
        }
      }
      return std::pair{state_to_history, history_count};
    }
    static constexpr auto build_result = build ();
    static constexpr auto map = build_result.first;
    static constexpr std::size_t count = build_result.second;
  };

  static constexpr bool has_history = (history_map_t::count > 0);

  template <std::size_t StateId>
  struct completion_chain {
    static consteval auto build() {
      std::array<std::size_t, normalized_model.transition_count> result{};
      std::size_t chain_count = 0;
      for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
        const auto &trans = normalized_model.transitions[t];
        if (trans.source_id == StateId &&
            trans.event_id == detail::invalid_index &&
            trans.timer_type == detail::timer_kind::none &&
            trans.type != detail::transition_kind::local) {
          result[chain_count++] = t;
        }
      }
      return std::pair{result, chain_count};
    }
    static constexpr auto build_result = build ();
    static constexpr std::size_t count = build_result.second;
    static constexpr auto ids = build_result.first;

    template <typename Fun>
    static constexpr void for_each(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(std::integral_constant<std::size_t, ids[Is]>{}),
         ...);
      }(std::make_index_sequence<count>{});
    }
  };

  template <std::size_t StateId>
  struct state_timers {
    // Find all transitions in this state that have a timer
    static consteval auto build() {
      std::array<std::size_t, normalized_model.transition_count> result{};
      std::size_t timer_count = 0;
      for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
        const auto &trans = normalized_model.transitions[t];
        if (trans.source_id == StateId &&
            trans.timer_type != detail::timer_kind::none) {
          result[timer_count++] = t;
        }
      }
      return std::pair{result, timer_count};
    }

    static constexpr auto build_result = build ();
    static constexpr std::size_t count = build_result.second;
    static constexpr auto t_ids = build_result.first;

    template <typename Fun>
    static constexpr void for_each(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(
          std::integral_constant<std::size_t, t_ids[Is]>{}
         ),
         ...);
      }(std::make_index_sequence<count>{});
    }
  };

  // 3a. Per-state and per-transition trait helpers
  template <std::size_t StateId>
  struct state_traits {
    static constexpr auto state = normalized_model.states[StateId];

    static constexpr bool has_entry =
      state.entry_start != detail::invalid_index;
    static constexpr bool has_exit = state.exit_start != detail::invalid_index;
    static constexpr bool has_activities =
      state.activity_start != detail::invalid_index;
    static constexpr bool has_timers = state_timers<StateId>::count > 0;
    static constexpr bool has_deferred = state.defer_count > 0;
    static constexpr bool has_completion_transitions =
      completion_chain<StateId>::count > 0;

    static constexpr bool has_work_on_enter =
      has_entry || has_activities || has_timers;
    static constexpr bool has_work_on_exit =
      has_exit || has_activities || has_timers;
  };

  static constexpr bool has_any_entry = []() {
    bool any = false;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((any = any || state_traits<Is>::has_entry), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return any;
  }();

  static constexpr bool has_any_exit = []() {
    bool any = false;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((any = any || state_traits<Is>::has_exit), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return any;
  }();

  static constexpr bool has_any_activities = []() {
    bool any = false;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((any = any || state_traits<Is>::has_activities), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return any;
  }();

  static constexpr bool has_any_timers = []() {
    bool any = false;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((any = any || state_traits<Is>::has_timers), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return any;
  }();

  static constexpr bool has_any_entry_exit_or_actions =
    has_any_entry || has_any_exit || has_any_activities || has_any_timers;

  static constexpr bool has_completion = []() {
    bool any = false;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((any = any || state_traits<Is>::has_completion_transitions), ...);
    }(std::make_index_sequence<decltype(normalized_model)::state_count>{});
    return any;
  }();

  template <std::size_t StateId>
  struct state_transitions {
    static consteval auto build() {
      std::array<std::size_t, normalized_model.transition_count> result{};
      std::size_t trans_count = 0;
      for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
        if (normalized_model.transitions[t].source_id == StateId) {
          result[trans_count++] = t;
        }
      }
      return std::pair{result, trans_count};
    }
    static constexpr auto build_result = build ();
    static constexpr std::size_t count = build_result.second;
    static constexpr auto ids = build_result.first;

    template <typename Fun>
    static constexpr bool try_match(Fun &&fun) {
      return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return (std::forward<Fun> (fun)(std::integral_constant<std::size_t, ids[Is]>{}) || ...);
      }(std::make_index_sequence<count>{});
    }
  };

  // 4. Thunk Types & Functions

  // Helper to convert runtime index to compile-time index via binary search
  template <std::size_t Start, std::size_t End, typename F>
  static constexpr auto static_switch(std::size_t i, F &&f) {
    if constexpr (End - Start <= 1) {
      if (i == Start) {
        return std::forward<F> (f)(std::integral_constant<std::size_t, Start>{});
      }
      // Should not be reached if i is within bounds
      if constexpr (std::is_void_v<decltype(std::forward<F>(f)(std::integral_constant<std::size_t, Start>{}))>) {
          return;
      } else {
          return decltype(std::forward<F>(f)(std::integral_constant<std::size_t, Start>{})){};
      }
    } else {
      constexpr std::size_t Mid = Start + (End - Start) / 2;
      if (i < Mid) {
        return static_switch<Start, Mid> (i, std::forward<F> (f));
      } else {
        return static_switch<Mid, End> (i, std::forward<F> (f));
      }
    }
  }

  template <std::size_t Start, std::size_t Count, typename Fun>
  static constexpr void for_each_index(Fun &&fun) {
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      (std::forward<Fun> (fun)(
        std::integral_constant<std::size_t, Start + Is>{}
       ),
       ...);
    }(std::make_index_sequence<Count>{});
  }

  // Core behavior invocation helper shared by both direct behaviors and
  // operation-name based behaviors. This contains the full signature
  // compatibility matrix used throughout the HSM.
  template <typename Callable, typename E>
  static constexpr auto invoke_behavior(Callable &&f,
                                        SignalType &c,
                                        instance_type &i,
                                        const E &e) noexcept -> decltype(auto) {
    using FDec = std::decay_t<Callable>;
    using TargetInst = typename detail::extract_instance_type<FDec>::type;

    // By default, behaviors operate on the HSM's instance_type. extract_instance_type
    // can override this for free functions that take a distinct context type as
    // their first non-Signal parameter. For member functions we deliberately
    // leave TargetInst = void so they bind to instance_type (or any of its
    // bases) via std::invoke.
    using EffInst = std::conditional_t<!std::is_void_v<TargetInst>, TargetInst,
                                       instance_type>;

    auto &eff_i = static_cast<EffInst &>(i);

    // 1. Try direct signatures against this exact event type E. These are the
    //    primary forms used by most behaviors.
    if constexpr (std::is_invocable_v<Callable, SignalType &, EffInst &, const E &>) {
      return std::invoke(
        f,
        c,
        eff_i,
        e
      );
    } else if constexpr (std::is_invocable_v<Callable, EffInst &, const E &>) {
      return std::invoke(
        f,
        eff_i,
        e
      );
    } else if constexpr (std::is_invocable_v<Callable, EffInst &>) {
      return std::invoke(
        f,
        eff_i
      );
    } else if constexpr (std::is_invocable_v<Callable>) {
      return std::invoke(f);
    } else {
      // 2. Typed-event dispatch path: attempt to remap E to an explicit
      //    event parameter type encoded in Callable (used by typed-event tests
      //    and AnyEvent handlers).
      using ArgType = typename detail::extract_event_type<FDec>::type;

      if constexpr (!std::is_void_v<ArgType> && requires { ArgType::kind; }) {
        // Use static kind information only. Instance-level kind() is ignored
        // here so that adding a virtual kind() to EventBase does not disable
        // the typed-event resolution matrix.
        if constexpr (hsm::is_kind(
                        E::kind,
                        ArgType::kind
                      ) ||
                      ArgType::kind ==
                        static_cast<hsm::kind_t>(hsm::Kind::AnyEvent)) {
          // Event kinds are compatible; dispatch via ArgType. This preserves
          // the behavior matrix semantics used in the typed-event tests.
          if constexpr (std::is_invocable_v<Callable, EffInst &, SignalType &,
                                            const ArgType &>) {
            if constexpr (std::is_convertible_v<const E &, const ArgType &>) {
              return std::invoke (
                f,
                eff_i,
                c,
                static_cast<const ArgType &> (e)
              );
            } else if constexpr (std::is_default_constructible_v<ArgType>) {
              ArgType tmp{};
              return std::invoke (
                f,
                eff_i,
                c,
                tmp
              );
            } else {
              static_assert(
                detail::always_false<FDec>,
                "hsm handler expects an event parameter type that cannot be "
                "constructed from the dispatched event"
              );
            }
          } else if constexpr (std::is_invocable_v<Callable, EffInst &,
                                                   const ArgType &>) {
            if constexpr (std::is_convertible_v<const E &, const ArgType &>) {
              return std::invoke (
                f,
                eff_i,
                static_cast<const ArgType &> (e)
              );
            } else if constexpr (std::is_default_constructible_v<ArgType>) {
              ArgType tmp{};
              return std::invoke (
                f,
                eff_i,
                tmp
              );
            } else {
              static_assert(
                detail::always_false<FDec>,
                "hsm handler expects an event parameter type that cannot be "
                "constructed from the dispatched event"
              );
            }
          } else if constexpr (std::is_invocable_v<Callable, SignalType &, EffInst &,
                                            const ArgType &>) {
            if constexpr (std::is_convertible_v<const E &, const ArgType &>) {
              return std::invoke (
                f,
                c,
                eff_i,
                static_cast<const ArgType &> (e)
              );
            } else if constexpr (std::is_default_constructible_v<ArgType>) {
              ArgType tmp{};
              return std::invoke (
                f,
                c,
                eff_i,
                tmp
              );
            } else {
              static_assert(
                detail::always_false<FDec>,
                "hsm handler expects an event parameter type that cannot be "
                "constructed from the dispatched event"
              );
            }
          } else {
            static_assert(
              detail::always_false<FDec>,
              "hsm handler uses an event parameter type ArgType but does not "
              "match any supported signature (Signal&, Self&, const ArgType&) "
              "or (Self&, const ArgType&)"
            );
          }
        } else {
          // This handler targets a different event kind: it is structurally
          // valid but simply does not apply to this particular E. This is the
          // behavior relied on by the typed-event matrix tests.
          return detail::not_invoked{};
        }
      } else {
        // No usable event type could be deduced and none of the direct
        // signatures matched: there is no supported way to call this handler.
        static_assert(
          detail::always_false<FDec>,
          "hsm::invoke_typed: unsupported handler signature. Supported "
          "forms include:\n"
          "  - void f(Signal&, Self&, const Event&)\n"
          "  - void f(Self&, const Event&)\n"
          "  - void f(Self&)\n"
          "  - void f()"
        );
      }
    }
  }

  // Public entry that understands operation_action / operation_guard tags and
  // otherwise delegates to invoke_behavior.
  template <typename F, typename E>
  static constexpr auto invoke_typed(F &&f, SignalType &c, instance_type &i,
                                     const E &e) noexcept -> decltype(auto) {
    using FDec = std::decay_t<F>;

    if constexpr (detail::is_operation_action_v<FDec>) {
      // Resolve operation by name value and dispatch via static_switch so that
      // the callable type remains a compile-time constant.
      std::size_t idx = operation_index_from_value(f.name);
      static_switch<0, operation_count>(idx, [&](auto I_const) {
        constexpr std::size_t I = I_const;
        auto &op_desc = std::get<I>(operation_tuple);
        auto &fn = op_desc.callable;
        (void) invoke_behavior(fn, c, i, e);
      });
      // Actions ignore return value.
      return detail::not_invoked{};
    } else if constexpr (detail::is_operation_guard_v<FDec>) {
      std::size_t idx = operation_index_from_value(f.name);
      bool result = false;
      static_switch<0, operation_count>(idx, [&](auto I_const) {
        constexpr std::size_t I = I_const;
        auto &op_desc = std::get<I>(operation_tuple);
        auto &fn = op_desc.callable;
        using Callable = std::decay_t<decltype(fn)>;
        using R = decltype(invoke_behavior(std::declval<Callable &>(), c, i, e));
        if constexpr (std::is_same_v<R, detail::not_invoked>) {
          // Guard did not apply; leave result = false.
        } else if constexpr (!std::is_void_v<R>) {
          auto r = invoke_behavior(fn, c, i, e);
          result = static_cast<bool>(r);
        } else {
          // Void-returning operation is not usable as a guard; treat as false.
        }
      });
      return result;
    } else {
      return invoke_behavior(std::forward<F> (f), c, i, e);
    }
  }

  // Timer coroutine: handles after/every/at timer types using co_await
  // Returns TaskType for clock-aware deadline tracking
  // Pool reference is first parameter to enable pool-based frame allocation.
  template <std::size_t I, detail::timer_kind Kind, typename E>
  TaskType timer_coro(PoolType& pool [[maybe_unused]], SignalType& c, const E& e, HSM& self) {
    using Source = std::tuple_element_t<I, decltype(timer_tuple)>;

    auto get_result = [&](auto&& f, SignalType& ctx, instance_type& inst,
                          const E& ev) {
      return invoke_typed(
        std::forward<decltype(f)>(f),
        ctx,
        inst,
        ev
      );
    };

    auto& inst_ref = self.self();

    if constexpr (Kind == detail::timer_kind::after) {
      if constexpr (detail::is_attribute_timer_source_v<Source>) {
        using name_type = typename Source::name_type;
        constexpr std::size_t attr_idx =
          attribute_index_from_name_type<name_type>();
        static_assert(
          attr_idx != detail::invalid_index,
          "hsm::after(\"name\") / on_timeout(\"name\"): attribute with this name "
          "does not exist in the model"
        );

        using AttrT = std::tuple_element_t<attr_idx, attribute_storage_type>;
        static_assert(
          detail::is_duration_v<AttrT>,
          "hsm::after(\"name\") / on_timeout(\"name\"): attribute type must be a "
          "std::chrono::duration"
        );

        auto d = std::get<attr_idx>(self.attributes_);
        if (d.count() <= 0) {
          co_return;
        }

        co_await AwaitableType{
          std::chrono::duration_cast<typename ClockType::duration>(d),
          &c
        };
        if (!c.is_set()) {
          self.template dispatch_timer_event<I>();
        }
      } else {
        auto res = get_result(
          std::get<I>(timer_tuple),
          c,
          inst_ref,
          e
        );
        using RetType = decltype(res);

        if constexpr (!std::is_same_v<RetType, detail::not_invoked>) {
          static_assert(
            detail::is_duration_v<RetType>,
            "hsm::after(f) / on_timeout(f): callable must return a "
            "std::chrono::duration"
          );

          auto d = res;
          co_await AwaitableType{
            std::chrono::duration_cast<typename ClockType::duration>(d),
            &c
          };
          if (!c.is_set()) {
            self.template dispatch_timer_event<I>();
          }
        }
      }
    } else if constexpr (Kind == detail::timer_kind::every) {
      if constexpr (detail::is_attribute_timer_source_v<Source>) {
        using name_type = typename Source::name_type;
        constexpr std::size_t attr_idx =
          attribute_index_from_name_type<name_type>();
        static_assert(
          attr_idx != detail::invalid_index,
          "hsm::every(\"name\") / on_interval(\"name\"): attribute with this "
          "name does not exist in the model"
        );

        using AttrT = std::tuple_element_t<attr_idx, attribute_storage_type>;
        static_assert(
          detail::is_duration_v<AttrT>,
          "hsm::every(\"name\") / on_interval(\"name\"): attribute type must be "
          "a std::chrono::duration"
        );

        while (!c.is_set()) {
          auto d = std::get<attr_idx>(self.attributes_);
          if (d.count() <= 0) {
            break;
          }

          co_await AwaitableType{
            std::chrono::duration_cast<typename ClockType::duration>(d),
            &c
          };
          if (c.is_set()) break;
          self.template dispatch_timer_event<I>();
        }
      } else {
        auto res = get_result(
          std::get<I>(timer_tuple),
          c,
          inst_ref,
          e
        );
        using RetType = decltype(res);

        if constexpr (!std::is_same_v<RetType, detail::not_invoked>) {
          static_assert(
            detail::is_duration_v<RetType>,
            "hsm::every(f) / on_interval(f): callable must return a "
            "std::chrono::duration"
          );

          auto d = res;
          while (!c.is_set()) {
            co_await AwaitableType{
              std::chrono::duration_cast<typename ClockType::duration>(d),
              &c
            };
            if (c.is_set()) break;
            self.template dispatch_timer_event<I>();
          }
        }
      }
    } else if constexpr (Kind == detail::timer_kind::at) {
      if constexpr (detail::is_attribute_timer_source_v<Source>) {
        using name_type = typename Source::name_type;
        constexpr std::size_t attr_idx =
          attribute_index_from_name_type<name_type>();
        static_assert(
          attr_idx != detail::invalid_index,
          "hsm::at(\"name\") / on_timepoint(\"name\"): attribute with this "
          "name does not exist in the model"
        );

        using AttrT = std::tuple_element_t<attr_idx, attribute_storage_type>;
        static_assert(
          detail::is_time_point_v<AttrT>,
          "hsm::at(\"name\") / on_timepoint(\"name\"): attribute type must be "
          "a std::chrono::time_point"
        );

        using AttrClock = typename AttrT::clock;
        static_assert(
          std::is_same_v<AttrClock, ClockType>,
          "hsm::at(\"name\") / on_timepoint(\"name\"): attribute time_point "
          "clock must match the HSM Clock type"
        );

        auto tp = std::get<attr_idx>(self.attributes_);
        auto now = ClockType::now();
        auto d = tp - now;
        if (d.count() > 0) {
          co_await AwaitableType{
            std::chrono::duration_cast<typename ClockType::duration>(d),
            &c
          };
        }
        if (!c.is_set()) self.template dispatch_timer_event<I>();
      } else {
        auto res = get_result(
          std::get<I>(timer_tuple),
          c,
          inst_ref,
          e
        );
        using TP = decltype(res);

        if constexpr (!std::is_same_v<TP, detail::not_invoked> &&
                      !std::is_same_v<TP, bool> && !std::is_void_v<TP> &&
                      !detail::is_duration_v<TP>) {
          static_assert(
            detail::is_time_point_v<TP>,
            "hsm::at(f) / on_timepoint(f): callable must return a "
            "std::chrono::time_point"
          );

          using CallClock = typename TP::clock;
          static_assert(
            std::is_same_v<CallClock, ClockType>,
            "hsm::at(f) / on_timepoint(f): time_point clock must match the "
            "HSM Clock type"
          );

          auto now = ClockType::now();
          auto d = res - now;
          if (d.count() > 0) {
            co_await AwaitableType{
              std::chrono::duration_cast<typename ClockType::duration>(d),
              &c
            };
          }
          if (!c.is_set()) self.template dispatch_timer_event<I>();
        }
      }
    }
    co_return;
  }

  struct ActiveTask {
    ActivityTask task;
    SignalType* signal;
  };

  // Timer tasks use TaskType for clock-aware deadline tracking
  struct TimerActiveTask {
    TaskType task;
    SignalType* signal;
  };

  // 5. Data Members
  SignalType* signal_{nullptr};

  // Engine state for thread-free cooperative scheduling
  SignalType wake_signal_{};  // Set when engine should wake (new events, etc.)
  std::optional<typename ClockType::time_point> next_deadline_;  // Earliest timer deadline
  bool terminated_{false};  // Set when HSM reaches final state
  std::optional<TaskType> engine_task_;  // The engine coroutine (if started)

  // Main event queue (for incoming and recalled events).
  //
  // Multi-producer (ISRs + tasks) single-consumer (engine) design:
  // - queue_.tail: producer-side enqueue cursor
  // - queue_.head: updated only by engine (single consumer), batched for perf
  // - processed_seq_: store-only by engine for result tracking
  //
  // Sequentially consistent ordering for MISRA compliance.
  Queue queue_{};

  // Separate deferred event pool (UML 2.5 compliant).
  // deferred_status events are moved here during processing and recalled to the
  // main queue when state changes. This ensures deferred events don't block
  // non-deferred events from being enqueued or processed.
  std::array<QueueEntry, queue_storage_size> deferred_queue_{};
  std::size_t deferred_head_{0};
  std::size_t deferred_tail_{0};

  // Sequence counters for dispatch result tracking.
  // enqueue_seq_: incremented on each event enqueue
  // processed_seq_: incremented after each macrostep completes
  std::atomic<std::uint64_t> enqueue_seq_{0};
  std::atomic<std::uint64_t> processed_seq_{0};

  // Results ring buffer - stores dispatch results indexed by sequence number.
  // Size matches queue to ensure results aren't overwritten before being read.
  std::array<detail::dispatch_status_t, queue_storage_size> results_{};

  std::array<SignalType, total_activity_count> activity_contexts_{};
  std::array<std::optional<ActiveTask>, total_activity_count> active_tasks_{};
  std::array<std::uint64_t, (total_activity_count + 63) / 64> active_tasks_mask_{};

  std::array<SignalType, total_timer_count> timer_contexts_{};
  std::array<std::optional<TimerActiveTask>, total_timer_count> active_timer_tasks_{};
  std::array<std::uint64_t, (total_timer_count + 63) / 64> active_timer_tasks_mask_{};

  // Coroutine frame pool - deferred init to avoid static initialization issues
  std::optional<PoolType> coro_pool_{};

  using attribute_storage_type =
    typename detail::make_attribute_storage<
      std::remove_cvref_t<decltype(attribute_tuple)>>::type;

  attribute_storage_type attributes_{};

  // For UML 2.5 history pseudostates we store, for each state, the most
  // recently active descendant leaf. This allows implementing both
  // shallow and deep history:
  //   - deep history: use the stored leaf directly
  //   - shallow history: map the stored leaf to the direct child of the
  //     composite and then follow its default initial chain
  std::array<std::size_t, history_map_t::count> last_active_leaf_{};

  std::size_t current_state_id_{detail::invalid_index};
  std::string_view id_{""};

public:
  // CRTP helper to obtain the derived Self instance.
  [[nodiscard]] constexpr instance_type &self() noexcept {
    return static_cast<instance_type &>(*this);
  }
  [[nodiscard]] constexpr const instance_type &self() const noexcept {
    return static_cast<const instance_type &>(*this);
  }

  // 6. Constructor & Destructor
  constexpr HSM() noexcept
    : signal_(nullptr),
      id_("") {
    init_data();
  }

  explicit constexpr HSM(std::string_view id) noexcept
    : signal_(nullptr),
      id_(id) {
    init_data();
  }

  explicit constexpr HSM(SignalType& signal) noexcept
    : signal_(&signal),
      id_("") {
    init_data();
  }

  constexpr HSM(SignalType& signal, std::string_view id) noexcept
    : signal_(&signal),
      id_(id) {
    init_data();
  }

  // Constructor taking only attribute override descriptors:
  //   HSM(set<"value">(123), set<"flag">(true))
  template <typename... Inits>
  explicit constexpr HSM(Inits&&... inits)
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
    : HSM() // delegate to default constructor (runs init_data())
  {
    (apply_initial_attr(std::forward<Inits>(inits)), ...);
  }

  // Constructor taking id plus attribute overrides:
  //   HSM("id", set<"value">(123), ...)
  template <typename... Inits>
  explicit constexpr HSM(std::string_view id, Inits&&... inits)
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
    : HSM(id)
  {
    (apply_initial_attr(std::forward<Inits> (inits)), ...);
  }

  // Constructor taking Signal plus attribute overrides:
  //   HSM(signal, set<"value">(123), ...)
  template <typename... Inits>
  explicit constexpr HSM(SignalType &signal, Inits&&... inits)
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
    : HSM(signal)
  {
    (apply_initial_attr(std::forward<Inits> (inits)), ...);
  }

  // Constructor taking Signal, id, plus attribute overrides:
  //   HSM(signal, "id", set<"value">(123), ...)
  template <typename... Inits>
  constexpr HSM(SignalType& signal, std::string_view id, Inits&&... inits)
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
    : HSM(signal, id)
  {
    (apply_initial_attr(std::forward<Inits>(inits)), ...);
  }

  // Static factory methods: construct and immediately start the HSM.
  // These provide a convenient one-liner for creating a ready-to-use HSM.
  //
  // Usage:
  //   auto hsm = Machine::create();
  //   auto hsm = Machine::create("id");
  //   auto hsm = Machine::create(signal);
  //   auto hsm = Machine::create(signal, "id");
  //   auto hsm = Machine::create(set<"attr">(value));

  [[nodiscard]] static Self create() {
    Self hsm;
    hsm.start();
    return hsm;
  }

  [[nodiscard]] static Self create(std::string_view id) {
    Self hsm(id);
    hsm.start();
    return hsm;
  }

  [[nodiscard]] static Self create(SignalType& signal) {
    Self hsm;
    hsm.start(signal);
    return hsm;
  }

  [[nodiscard]] static Self create(SignalType& signal, std::string_view id) {
    Self hsm(id);
    hsm.start(signal);
    return hsm;
  }

  // Factory with attribute overrides
  template <typename... Inits>
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
  [[nodiscard]] static Self create(Inits&&... inits) {
    Self hsm(std::forward<Inits>(inits)...);
    hsm.start();
    return hsm;
  }

  // Factory with id and attribute overrides
  template <typename... Inits>
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
  [[nodiscard]] static Self create(std::string_view id, Inits&&... inits) {
    Self hsm(id, std::forward<Inits>(inits)...);
    hsm.start();
    return hsm;
  }

  // Factory with signal and attribute overrides
  template <typename... Inits>
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
  [[nodiscard]] static Self create(SignalType& signal, Inits&&... inits) {
    Self hsm(std::forward<Inits>(inits)...);
    hsm.start(signal);
    return hsm;
  }

  // Factory with signal, id, and attribute overrides
  template <typename... Inits>
    requires (sizeof...(Inits) > 0 && (detail::is_initial_attr_v<Inits> && ...))
  [[nodiscard]] static Self create(SignalType& signal, std::string_view id, Inits&&... inits) {
    Self hsm(id, std::forward<Inits>(inits)...);
    hsm.start(signal);
    return hsm;
  }

private:
  // Initialize data structures without entering initial state.
  // Called by constructors. Does NOT start the machine.
  constexpr void init_data() noexcept {
    if constexpr (has_history) {
      last_active_leaf_.fill(detail::invalid_index);
    }

    // Initialize attributes from declared defaults (if any)
    if constexpr (attribute_count > 0) {
      for_each_index<0, attribute_count>(
        [&](auto I) {
          constexpr std::size_t idx = I;
          using desc_type = std::tuple_element_t<idx, decltype(attribute_tuple)>;
          if constexpr (desc_type::has_default) {
            std::get<idx>(attributes_) = std::get<idx>(attribute_tuple).default_value;
          }
        });
    }

    // Initialize queue state (ready to accept pre-start events)
    queue_.Clear();
    current_state_id_ = detail::invalid_index;  // Not started yet
  }

  // Enter initial state and start dispatch activity.
  // Called by start() methods. Idempotent - safe to call multiple times.
  void start_impl() noexcept {
    // Idempotent: only start once
    if (current_state_id_ != detail::invalid_index) {
      return;
    }

    current_state_id_ = 0;  // Root

    // Enter root state
    SignalType signal{};
    if (signal_) signal.reset(signal_);
    InitialEvent e{};
    enter_state_impl<0>(signal, e);

    // Follow initial transitions to reach initial leaf state
    resolve_initial_compile_time<0>(signal, e);

    // Process any events queued before start()
    drain_queue_events();
  }

public:
  ~HSM() {
    // Cancel all active tasks to unblock threads waiting on contexts
    // Use bitmasks to iterate only active slots (O(1) relative to total capacity)
    for (std::size_t w = 0; w < active_tasks_mask_.size(); ++w) {
         if (std::uint64_t v = active_tasks_mask_[w]) {
             while (v) {
                 int bit = std::countr_zero(v);
                 std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
                 if (active_tasks_[idx].has_value()) {
                     active_tasks_[idx]->signal->set();
                 }
                 v &= ~(1ULL << bit);
             }
         }
    }
    for (std::size_t w = 0; w < active_timer_tasks_mask_.size(); ++w) {
         if (std::uint64_t v = active_timer_tasks_mask_[w]) {
             while (v) {
                 int bit = std::countr_zero(v);
                 std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
                 if (active_timer_tasks_[idx].has_value()) {
                     active_timer_tasks_[idx]->signal->set();
                 }
                 v &= ~(1ULL << bit);
             }
         }
    }
    // Member destructors will join tasks now that they are signalled
  }

  // Start the HSM engine - returns a Task that can be:
  // - Fire-and-forget: auto task = sm.start();
  // - Blocking await: sm.start().await();
  // - Coroutine await: co_await sm.start();
  //
  // The engine enters initial state, processes events, and manages timers.
  // Events dispatched before start() are queued and processed after
  // initial state entry. This method is idempotent.
  TaskType start() noexcept {
    // Idempotent: return a completed (empty) task if already started
    if (started()) {
      return TaskType{};
    }
    // Initialize coroutine frame pool (deferred to avoid static init issues)
    if (!coro_pool_) {
      coro_pool_.emplace();
    }
    auto task = start_engine(*coro_pool_);
    task.resume();  // Initialize the HSM synchronously
    return task;
  }

  // Start the HSM with an external cancellation signal.
  // The signal can be used to cancel long-running activities/timers.
  TaskType start(SignalType& signal) noexcept {
    // Idempotent: return a completed (empty) task if already started
    if (started()) {
      return TaskType{};
    }
    signal_ = &signal;
    // Initialize coroutine frame pool (deferred to avoid static init issues)
    if (!coro_pool_) {
      coro_pool_.emplace();
    }
    auto task = start_engine(*coro_pool_);
    task.resume();  // Initialize the HSM synchronously
    return task;
  }

private:
  // Engine coroutine - initializes HSM and runs the cooperative scheduler loop.
  // The loop suspends on Awaitable (deadline + wake signal) and resumes when:
  // - A deadline passes (timer fires)
  // - dispatch() sets wake_signal_ (event queued)
  // The returned Task can be driven externally via task.resume().
  // Pool reference is first parameter to enable pool-based frame allocation.
  TaskType start_engine(PoolType& pool [[maybe_unused]]) noexcept {
    // Idempotent: only start once
    if (current_state_id_ != detail::invalid_index) {
      co_return;
    }

    current_state_id_ = 0;  // Root

    // Enter root state
    SignalType signal{};
    if (signal_) signal.reset(signal_);
    InitialEvent e{};
    enter_state_impl<0>(signal, e);

    // Follow initial transitions to reach initial leaf state
    resolve_initial_compile_time<0>(signal, e);

    // Process any events queued before start()
    drain_queue_events();

    // Compute initial deadline from timers started during state entry
    recompute_next_deadline();

    // Engine main loop - cooperative scheduling
    // Each resume() iteration processes ready timers/activities and events
    while (!terminated_) {
      // Suspend until deadline passes or wake signal is set
      co_await AwaitableType{next_deadline_, &wake_signal_};
      wake_signal_.reset();

      // Resume coroutines whose deadlines have passed
      auto now = ClockType::now();
      resume_ready_timers(now);
      resume_ready_activities(now);

      // Process any queued events
      drain_queue_events();

      // Recompute next wake deadline
      recompute_next_deadline();
    }
  }

  // Resume timer coroutines whose deadlines have passed
  void resume_ready_timers(typename ClockType::time_point now) noexcept {
    for (std::size_t w = 0; w < active_timer_tasks_mask_.size(); ++w) {
      if (std::uint64_t v = active_timer_tasks_mask_[w]) {
        while (v) {
          int bit = std::countr_zero(v);
          std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
          if (active_timer_tasks_[idx].has_value()) {
            auto& task = active_timer_tasks_[idx]->task;
            // Only resume if task is not done AND deadline has passed
            if (!task.done()) {
              auto deadline = task.deadline();
              if (!deadline || now >= *deadline) {
                task.resume();
              }
            }
          }
          v &= ~(1ULL << bit);
        }
      }
    }
  }

  // Resume activity coroutines that are ready
  void resume_ready_activities(typename ClockType::time_point now [[maybe_unused]]) noexcept {
    for (std::size_t w = 0; w < active_tasks_mask_.size(); ++w) {
      if (std::uint64_t v = active_tasks_mask_[w]) {
        while (v) {
          int bit = std::countr_zero(v);
          std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
          if (active_tasks_[idx].has_value()) {
            auto& task = active_tasks_[idx]->task;
            if (!task.done()) {
              task.resume();
            }
          }
          v &= ~(1ULL << bit);
        }
      }
    }
  }

  // Recompute the next deadline by scanning active timer tasks
  void recompute_next_deadline() noexcept {
    next_deadline_.reset();
    // Scan active timer tasks for the nearest deadline
    for (std::size_t w = 0; w < active_timer_tasks_mask_.size(); ++w) {
      if (std::uint64_t v = active_timer_tasks_mask_[w]) {
        while (v) {
          int bit = std::countr_zero(v);
          std::size_t idx = w * 64 + static_cast<std::size_t>(bit);
          if (active_timer_tasks_[idx].has_value()) {
            auto& task = active_timer_tasks_[idx]->task;
            if (!task.done()) {
              auto deadline = task.deadline();
              if (deadline) {
                if (!next_deadline_ || *deadline < *next_deadline_) {
                  next_deadline_ = deadline;
                }
              }
            }
          }
          v &= ~(1ULL << bit);
        }
      }
    }
  }

public:

  // Check if the HSM has been started.
  [[nodiscard]] constexpr bool started() const noexcept {
    return current_state_id_ != detail::invalid_index;
  }

  // 7. Accessor
  // constexpr void set_id(std::string_view id) noexcept { id_ = id; }
  [[nodiscard]] constexpr std::string_view id() const noexcept override { return id_; }

  [[nodiscard]] constexpr std::string_view state() const noexcept override {
    if (current_state_id_ == detail::invalid_index) return "";
    return normalized_model.get_state_name (current_state_id_);
  }

  [[nodiscard]] snapshot_type takeSnapshot() const noexcept {
    snapshot_type snapshot{};
    snapshot.ID = id();
    snapshot.QualifiedName = normalized_model.get_state_name(0);
    snapshot.State = state();
    snapshot.QueueLen = queue_.Len() + (deferred_tail_ - deferred_head_);

    append_snapshot_attributes(snapshot);
    append_snapshot_queue_events(snapshot);
    return snapshot;
  }

  [[nodiscard]] snapshot_type TakeSnapshot() const noexcept {
    return takeSnapshot();
  }

  // 8. Public Methods
  // Compile-time capability query: does this HSM know how to handle
  // events of type E (including via AnyEvent wildcard)? This is used by
  // higher-level helpers like Group to avoid instantiating dispatch<E>()
  // for machines that cannot possibly handle E.
  template <typename E>
  static consteval bool supports_event() {
    if constexpr (requires { E::kind; }) {
      constexpr auto kind = E::kind;
      constexpr std::size_t id = event_index<kind>();
      return id != detail::invalid_index;
    } else {
      return false;
    }
  }

  template <detail::fixed_string Name>
  static consteval bool supports_event() {
    constexpr auto hash = detail::fnv1a_64(Name.view());
    constexpr auto k = hsm::make_kind(
      hash,
      hsm::Kind::Event
    );
    return event_index<k>() != detail::invalid_index;
  }

  // Compile-time capability query: does this HSM define an
  // operation with the given compile-time name?
  template <detail::fixed_string Name>
  static consteval bool supports_operation() {
    return operation_index<Name>() != detail::invalid_index;
  }

  template <typename T>
  void dispatch() noexcept {
    // Compile-time lookup for typed events
    constexpr auto kind = T::kind;
    constexpr std::size_t id = event_index<kind>();
    (void)dispatch_typed_by_id<id>(T{});
  }

  template <typename T>
  void dispatch(const T &e) noexcept {
    // Compile-time lookup for typed events
    constexpr auto kind = T::kind;
    constexpr std::size_t id = event_index<kind>();
    (void)dispatch_typed_by_id<id>(e);
  }

  // Synchronous dispatch: process event inline without queue or coroutine.
  // This is the fast path for single-threaded / manual-drive usage where
  // the caller owns the run loop and does not need ISR-safety or
  // cross-thread dispatch.  Bypasses the event queue, atomics, variant
  // storage, and coroutine resume/suspend entirely.
  template <typename T>
  constexpr void process() noexcept {
    constexpr auto kind = T::kind;
    constexpr std::size_t id = event_index<kind>();
    (void)dispatch_typed_by_id_core<id>(T{});
  }

  template <typename T>
  constexpr void process(const T &e) noexcept {
    constexpr auto kind = T::kind;
    constexpr std::size_t id = event_index<kind>();
    (void)dispatch_typed_by_id_core<id>(e);
  }

  // Operation invocation API. Resolves the named operation at
  // compile time, dispatches the corresponding CallEvent through
  // the state machine, and then invokes the bound callable.
  template <detail::fixed_string Name, typename... Args>
  constexpr decltype(auto) call(Args &&... args) noexcept {
    // 1. Resolve operation descriptor at compile time
    constexpr std::size_t op_idx = operation_index<Name>();
    static_assert(
      op_idx != detail::invalid_index,
      "hsm::HSM::call<Name>() requires an operation(Name, ...) "
      "declaration in the model"
    );

    using ops_tuple_type = std::remove_cvref_t<decltype(operation_tuple)>;
    using OpDesc = std::tuple_element_t<op_idx, ops_tuple_type>;
    using Callable = typename OpDesc::callable_type;

    static_assert(
      detail::is_supported_operation_v<Callable>,
      "hsm::HSM::call<Name>() operation callable type is not supported"
    );

    using Sig = detail::operation_signature<Callable>;
    using ParamTuple = typename Sig::args_tuple;

    static_assert(
      std::tuple_size_v<ParamTuple> == sizeof...(Args),
      "hsm::HSM::call<Name>() argument count does not match operation signature"
    );

    using ProvidedTuple = std::tuple<std::decay_t<Args>...>;

    constexpr bool args_compatible = []<std::size_t... Is>(std::index_sequence<Is...>) {
      return (std::is_convertible_v<
                std::tuple_element_t<Is, ProvidedTuple>,
                std::tuple_element_t<Is, ParamTuple>> && ...);
    }(std::make_index_sequence<std::tuple_size_v<ParamTuple>>{});

    static_assert(
      args_compatible,
      "hsm::HSM::call<Name>() argument types are not compatible with the "
      "operation signature"
    );

    // 3. Compute CallEvent kind and event index
    constexpr auto hash = detail::fnv1a_64(Name.view());
    constexpr auto k = hsm::make_kind(
      hash,
      hsm::Kind::CallEvent
    );
    constexpr std::size_t EventId = event_index<k>();

    using EventArgsTuple = std::tuple<std::decay_t<Args>...>;
    using EventType = detail::call_event<Name, EventArgsTuple>;
    EventType evt{};
    evt.args = EventArgsTuple{ std::forward<Args> (args)... };

    // Dispatch the CallEvent synchronously (inline) before invoking the
    // operation body. This ensures the event-driven transition (on_call effect)
    // runs before the operation body executes. We use dispatch_typed_by_id_core
    // directly instead of the queue-based dispatch_typed_by_id because:
    // 1. call() is inherently synchronous (returns a value)
    // 2. The CallEvent MUST be processed before the body runs
    // 3. call() is not ISR-safe anyway (invokes user code)
    if constexpr (EventId != detail::invalid_index) {
      (void) dispatch_typed_by_id_core<EventId>(evt);
    }

    auto &op_desc = std::get<op_idx> (operation_tuple);
    auto &fn = op_desc.callable;

    if constexpr (Sig::is_member) {
      return std::invoke(
        fn,
        self (),
        std::forward<Args> (args)...
      );
    } else {
      return std::invoke(
        fn,
        std::forward<Args> (args)...
      );
    }
  }

  // Name-keyed attribute accessors using fixed_string literals.
  template <detail::fixed_string Name>
  [[nodiscard]] constexpr auto &get() noexcept {
    constexpr std::size_t idx = attribute_index<Name>();
    static_assert(idx != detail::invalid_index,
                  "hsm::HSM::get<Name>() requires an attribute declared with that name");
    return std::get<idx>(attributes_);
  }

  // Runtime name-keyed attribute accessor: returns std::any holding a copy
  // of the attribute value, or an empty std::any if no attribute matches.
  [[nodiscard]] std::any get(std::string_view name) noexcept {
    std::any result;
    if constexpr (attribute_count > 0) {
      for_each_index<0, attribute_count>([&](auto I) {
        constexpr std::size_t idx = I;
        constexpr auto desc = std::get<idx>(attribute_tuple);
        if (desc.name.view() == name) {
          result = std::get<idx>(attributes_);
        }
      });
    }
    return result;
  }

  // Runtime attribute mutation: update storage and emit ChangeEvent-kind
  // events that drive when("name") transitions.
  template <detail::fixed_string Name, typename V>
  void set(V &&value) noexcept {
    constexpr std::size_t idx = attribute_index<Name>();
    static_assert(idx != detail::invalid_index,
                  "hsm::HSM::set<Name>() requires an attribute declared with that name");

    using T = std::tuple_element_t<idx, attribute_storage_type>;
    auto &slot = std::get<idx>(attributes_);

    T new_value = static_cast<T>(std::forward<V> (value));

    bool changed = true;
    if constexpr (requires(const T &a, const T &b) { a != b; }) {
    if constexpr (std::is_floating_point_v<T>) {
      changed = std::abs(slot - new_value) > std::numeric_limits<T>::epsilon();
    } else if constexpr (requires(const T &a, const T &b) { a != b; }) {
      changed = (slot != new_value);
    }
    }
    if (!changed) return;

    slot = new_value;

    // Emit a name-based ChangeEvent-kind event if the model declares
    // any transitions listening for this attribute's change via
    // when("name"). The event kind is derived from the attribute
    // name and the ChangeEvent base kind.
    constexpr auto hash = detail::fnv1a_64(Name.view());
    constexpr auto k = hsm::make_kind(hash, hsm::Kind::ChangeEvent);
    constexpr std::size_t id = event_index<k>();
    if constexpr (id != detail::invalid_index) {
      using E = Event<k>;
      (void)dispatch_typed_by_id<id>(E{});
    }
  }

  // Runtime name-keyed attribute mutation using std::any for type erasure.
  void set(std::string_view name, const std::any& value) noexcept {
    if constexpr (attribute_count == 0) {
      (void)name; (void)value;
    } else {
      bool found = false;
      for_each_index<0, attribute_count>([&](auto I) {
        if (found) return;
        constexpr std::size_t idx = I;
        constexpr auto desc = std::get<idx>(attribute_tuple);
        if (desc.name.view() != name) return;
        found = true;
        using T = std::tuple_element_t<idx, attribute_storage_type>;

        const T* ptr = std::any_cast<T>(&value);
        if (!ptr) return;

        auto& slot = std::get<idx>(attributes_);
        T new_value = *ptr;

        bool changed = true;
        if constexpr (requires(const T& a, const T& b) { a != b; }) {
          if constexpr (std::is_floating_point_v<T>) {
            changed = std::abs(slot - new_value) > std::numeric_limits<T>::epsilon();
          } else {
            changed = (slot != new_value);
          }
        }
        if (!changed) return;

        slot = new_value;

        constexpr auto hash = detail::fnv1a_64(desc.name.view());
        constexpr auto k = hsm::make_kind(hash, hsm::Kind::ChangeEvent);
        constexpr std::size_t event_id = event_index<k>();
        if constexpr (event_id != detail::invalid_index) {
          using E = Event<k>;
          (void)dispatch_typed_by_id<event_id>(E{});
        }
      });
    }
  }

  // Constructor-time attribute initialization helper: override attribute
  // storage without emitting any events.
  template <detail::fixed_string Name, typename V>
  constexpr void set_initial(V &&value) noexcept {
    constexpr std::size_t idx = attribute_index<Name>();
    static_assert(idx != detail::invalid_index,
                  "hsm::HSM::set_initial<Name>() requires an attribute declared with that name");

    using T = std::tuple_element_t<idx, attribute_storage_type>;
    auto &slot = std::get<idx>(attributes_);
    slot = static_cast<T>(std::forward<V> (value));
  }

  template <detail::fixed_string Name, typename U>
  constexpr void apply_initial_attr(const hsm::initial_attr<Name, U> &init) noexcept {
    set_initial<Name>(init.value);
  }

  // Dispatcher interface implementation: runtime-polymorphic entry point
  // that bridges EventBase& to the existing strongly-typed dispatch
  // machinery using the model's normalized event table.
  // Helper used by the Dispatcher interface to walk the compile-time
  // event type list and find the concrete type matching a runtime kind.
  // O(1) Dispatch Logic
  static constexpr std::size_t dispatch_map_capacity = 
      (normalized_model.event_count == 0) ? 1 : (normalized_model.event_count * 2);

  using DispatchMap = detail::fixed_map<hsm::kind_t, std::size_t, dispatch_map_capacity>;

  struct KindToEventIdMap {
    static consteval auto build() {
      DispatchMap m{};
      for_each_index<0, normalized_model.event_count>([&](auto I){
            using E = std::tuple_element_t<I.value, std::remove_cvref_t<decltype(event_types_tuple)>>;
            if constexpr (!std::is_same_v<E, PlaceholderEvent>) {
                 (void)m.insert(normalized_model.events[I.value].kind, I.value);
            }
      });
      return m;
    }
    static constexpr auto map = build();
  };

  // Fire-and-forget enqueue for polymorphic dispatch (ISR-safe)
  // Just enqueues and returns immediately - does NOT wait for processing
  template <std::size_t EventId, typename E>
  detail::dispatch_status_t enqueue_typed_by_id(const E &e) noexcept {
    static_assert(
      EventId != detail::invalid_index,
      "enqueue_typed_by_id used with invalid EventId"
    );

    if constexpr (!can_enqueue_event_v<EventId, E> && !is_any_event_slot_v<EventId>) {
      (void)e;
      return detail::queue_full_status;
    } else {
      // Increment sequence and enqueue
      enqueue_seq_.fetch_add(1, std::memory_order_acq_rel);
      if constexpr (can_enqueue_event_v<EventId, E>) {
        return enqueue_event<EventId>(e) ? detail::processed_status : detail::queue_full_status;
      } else {
        // Wildcard slot: substitute AnyEvent{}, mark as fallback.
        // Fallback events are NOT subject to defer<AnyEvent>() deferral.
        // This ensures typed events like WildEvent don't get deferred
        // when dispatched via AnyEvent fallback slot.
        return enqueue_event<EventId>(AnyEvent{}, /*is_fallback=*/true) ? detail::processed_status : detail::queue_full_status;
      }
    }
  }

  using DispatchThunk = void (HSM::*)(const EventBase &);

  template <std::size_t I>
  void dispatch_thunk_impl(const EventBase &e) {
      using E = std::tuple_element_t<I, std::remove_cvref_t<decltype(event_types_tuple)>>;
      if constexpr (std::is_same_v<E, PlaceholderEvent> || !std::is_base_of_v<EventBase, E>) {
          (void)e;
      } else {
          (void)this->template dispatch_typed_by_id<I>(static_cast<const E&>(e));
      }
  }

  static constexpr auto dispatch_thunks = []() {
      std::array<DispatchThunk, (normalized_model.event_count > 0 ? normalized_model.event_count : 1)> arr{};
      if constexpr (normalized_model.event_count > 0) {
        for_each_index<0, normalized_model.event_count>([&](auto I){
            arr[I.value] = &HSM::dispatch_thunk_impl<I.value>;
        });
      }
      return arr;
  }();

  // Polymorphic dispatch.
  void dispatch(const EventBase &e) override {
      const auto k = e.kind_value();
      const std::size_t* id_ptr = KindToEventIdMap::map.find(k);

      if (id_ptr != nullptr) {
          (this->*dispatch_thunks[*id_ptr])(e);
      }
  }

  template <detail::fixed_string Name, typename... U>
  constexpr void apply_initial_attr(const hsm::initial_emplace<Name, U...> &init) noexcept {
    constexpr std::size_t idx = attribute_index<Name>();
    static_assert(idx != detail::invalid_index,
                  "hsm::HSM::emplace<Name>() requires an attribute declared with that name");

    using T = std::tuple_element_t<idx, attribute_storage_type>;
    auto &slot = std::get<idx>(attributes_);

    slot = std::apply(
      [](auto const &... xs) {
        return T(xs...);
      },
      init.args
    );
  }

  template <detail::fixed_string Name>
  void dispatch() noexcept {
    // Compute kind the same way extract_all_events_nttp does for on("literal")
    constexpr auto hash = detail::fnv1a_64(Name.view());
    constexpr auto k = hsm::make_kind(hash, hsm::Kind::Event);
    using E = Event<k>;
    dispatch<E>();
  }

private:
  static constexpr std::string_view transition_target_for_event(
      std::size_t event_id) noexcept {
    for (const auto &transition : normalized_model.transitions) {
      if (transition.event_id == event_id &&
          transition.target_id != detail::invalid_index) {
        return normalized_model.get_state_name(transition.target_id);
      }
    }
    return {};
  }

  static constexpr bool transition_guard_for_event(std::size_t event_id) noexcept {
    for (const auto &transition : normalized_model.transitions) {
      if (transition.event_id == event_id) {
        return transition.guard_idx != detail::invalid_index;
      }
    }
    return false;
  }

  static constexpr EventSnapshot snapshot_event_for_id(std::size_t event_id) noexcept {
    EventSnapshot event{};
    if (event_id < normalized_model.event_count) {
      event.Name = normalized_model.get_event_name(event_id);
      event.Kind = normalized_model.events[event_id].kind;
      event.Target = transition_target_for_event(event_id);
      event.Guard = transition_guard_for_event(event_id);
    }
    return event;
  }

  void append_snapshot_event(snapshot_type &snapshot,
                             const QueueEntry &entry) const noexcept {
    if (snapshot.EventLen >= snapshot.Events.size()) return;
    snapshot.Events[snapshot.EventLen] = snapshot_event_for_id(entry.event_id);
    ++snapshot.EventLen;
  }

  void append_snapshot_queue_events(snapshot_type &snapshot) const noexcept {
    const auto head = queue_.head.load(std::memory_order_seq_cst);
    const auto tail = queue_.tail.load(std::memory_order_seq_cst);
    for (std::size_t seq = head; seq < tail; ++seq) {
      append_snapshot_event(snapshot, queue_.entries[seq % queue_capacity]);
    }
    for (std::size_t seq = deferred_head_; seq < deferred_tail_; ++seq) {
      append_snapshot_event(snapshot, deferred_queue_[seq % queue_capacity]);
    }
  }

  template <std::size_t I>
  void append_snapshot_attribute(snapshot_type &snapshot) const noexcept {
    if (snapshot.AttributeLen >= snapshot.Attributes.size()) return;

    constexpr auto desc = std::get<I>(attribute_tuple);
    auto &attribute = snapshot.Attributes[snapshot.AttributeLen];
    const auto root = normalized_model.get_state_name(0);
    std::size_t len = 0;

    for (char ch : root) {
      if (len >= attribute.Name.size()) break;
      attribute.Name[len++] = ch;
    }
    if (len < attribute.Name.size()) {
      attribute.Name[len++] = '/';
    }
    for (char ch : desc.name.view()) {
      if (len >= attribute.Name.size()) break;
      attribute.Name[len++] = ch;
    }

    attribute.NameLen = len;
    attribute.Value = &std::get<I>(attributes_);
    ++snapshot.AttributeLen;
  }

  void append_snapshot_attributes(snapshot_type &snapshot) const noexcept {
    if constexpr (attribute_count > 0) {
      for_each_index<0, attribute_count>([&](auto I) {
        append_snapshot_attribute<I.value>(snapshot);
      });
    }
  }

  // --- Unified queue helpers -------------------------------------------------

  [[nodiscard]] bool queue_empty() const noexcept {
    return queue_.Len() == 0;
  }

  [[nodiscard]] bool queue_full() const noexcept {
    return queue_.Full();
  }

  template <std::size_t EventId, typename E>
  requires (can_enqueue_event_v<EventId, E>)
  bool enqueue_event(const E &e, bool is_fallback = false) noexcept {
    // Main queue is single-consumer and intended for a single logical
    // producer; concurrent producers must provide external serialization.
    QueueEntry entry{};
    entry.event.template emplace<EventId>(e);
    entry.event_id = EventId;
    entry.is_fallback = is_fallback;
    if (!queue_.Push(entry)) {
      return false;
    }

    // Wake the engine so it processes the new event
    wake_signal_.set();
    return true;
  }

  // Check if deferred queue is full
  [[nodiscard]] bool deferred_queue_full() const noexcept {
    return (deferred_tail_ - deferred_head_) >= queue_capacity;
  }

  // Enqueue an event to the deferred queue
  template <std::size_t EventId, typename E>
  bool enqueue_deferred(const E &e) noexcept {
    if (deferred_queue_full()) {
      return false;  // deferred_status queue overflow - drop the event
    }
    const std::size_t index = deferred_tail_ % queue_capacity;
    deferred_queue_[index].event.template emplace<EventId>(e);
    deferred_queue_[index].event_id = EventId;
    deferred_tail_++;
    return true;
  }

  // Enqueue a complete queue entry to the deferred queue (preserves event_id)
  bool enqueue_deferred_entry(const QueueEntry &entry) noexcept {
    if (deferred_queue_full()) {
      return false;
    }
    const std::size_t index = deferred_tail_ % queue_capacity;
    deferred_queue_[index] = entry;
    deferred_tail_++;
    return true;
  }

  // Recall deferred events back to main queue after state change.
  // Events that are still deferred in the new state go back to deferred queue.
  void recall_deferred_events() noexcept {
    if constexpr (!has_deferred_events) return;

    // Move all deferred events to main queue for reconsideration
    while (deferred_head_ < deferred_tail_) {
      const std::size_t index = deferred_head_ % queue_capacity;
      QueueEntry& entry = deferred_queue_[index];
      deferred_head_++;

      // Try to enqueue to main queue
      if (!queue_.Push(entry)) {
        // Main queue full - put back in deferred queue (at front)
        deferred_head_--;
        break;
      }
    }
  }

  // Drain all events from main queue. deferred_status events are moved to the
  // separate deferred queue. After state changes, deferred events are
  // recalled to the main queue for reconsideration.
  //
  // Atomic optimization: processed_seq_ uses store instead of fetch_add since
  // only the engine (single consumer) updates it.
  void drain_queue_events() noexcept {
    // Take a snapshot of the tail at entry. We will process up to this
    // index and ignore any events enqueued later in this macrostep.
    std::size_t limit = queue_.tail.load(std::memory_order_seq_cst);
    auto state_at_entry = current_state_id_;

    // Local shadow of processed_seq_ - only write to atomic once per event
    std::uint64_t seq = processed_seq_.load(std::memory_order_relaxed);

    for (;;) {
      QueueEntry entry{};
      if (!queue_.Pop(entry, limit)) {
        break;
      }

      // Stored event_id is used for timer events where kind-based lookup fails
      const std::size_t stored_event_id = entry.event_id;

      // Check if this event is deferred in current state.
      // Fallback events (typed events substituted as AnyEvent) are NEVER deferred,
      // even if defer<AnyEvent>() is declared. This ensures that only explicit
      // AnyEvent dispatches are subject to wildcard deferral.
      // Use static_switch on stored_event_id instead of std::visit for better performance
      bool is_event_deferred = false;
      if constexpr (has_deferred_events && normalized_model.event_count > 0) {
        if (!entry.is_fallback && stored_event_id < normalized_model.event_count) {
          static_switch<0, normalized_model.event_count>(
            stored_event_id,
            [this, &is_event_deferred](auto I) {
              is_event_deferred = this->is_deferred(current_state_id_, I.value);
            }
          );
        }
      }

      if (is_event_deferred) {
        // Move entire entry to deferred queue (preserves variant and event_id)
        (void) enqueue_deferred_entry(entry);

        queue_.CommitPop();

        // Mark as processed (deferred_status) - store instead of fetch_add (single consumer)
        results_[seq % queue_capacity] = detail::deferred_status;
        ++seq;
        processed_seq_.store(seq, std::memory_order_release);
        continue;
      }

      // Event is not deferred - process it
      // Use static_switch on stored_event_id instead of std::visit for better performance
      detail::dispatch_status_t result = detail::processed_status;
      if constexpr (normalized_model.event_count > 0) {
        if (stored_event_id < normalized_model.event_count) {
          static_switch<0, normalized_model.event_count>(
            stored_event_id,
            [this, &result, &entry](auto I) {
              using event_tuple_t = std::remove_cvref_t<decltype(event_types_tuple)>;
              using E = std::tuple_element_t<I.value, event_tuple_t>;
              if constexpr (!std::is_same_v<E, PlaceholderEvent>) {
                // Get typed event from variant using compile-time index
                const auto& evt = std::get<I.value>(entry.event);
                result = this->template dispatch_typed_by_id_core<I.value>(evt);
              }
            }
          );
        }
      }

      queue_.CommitPop();

      // Write result to ring buffer - store instead of fetch_add (single consumer)
      results_[seq % queue_capacity] = result;
      ++seq;
      processed_seq_.store(seq, std::memory_order_release);
      wake_signal_.set();

      // If state changed, recall deferred events for reconsideration
      if (current_state_id_ != state_at_entry) {
        state_at_entry = current_state_id_;
        recall_deferred_events();
        // Update limit to include recalled events
        limit = queue_.tail.load(std::memory_order_seq_cst);
      }
    }
  }

  template <std::size_t EventId, typename E>
  constexpr detail::dispatch_status_t dispatch_typed_by_id_core(const E &e) noexcept {
    // For typed dispatch we require that the event kind is actually present
    // in the model. Using an event type that never appears in the model is
    // a programming error and is rejected at compile time.
    static_assert(
      EventId != detail::invalid_index,
      "hsm::HSM::dispatch<T>() used with event type T whose kind is not present in the model"
    );

    SignalType signal{};
    if (signal_) signal.reset (signal_);

    // NOTE: Deferral check is handled by drain_queue_events() before calling
    // this function. This function only processes non-deferred events.

    if (current_state_id_ < normalized_model.state_count) {
      static_switch<0, normalized_model.state_count> (
        current_state_id_,
        [&](auto I) {
          return this->template dispatch_typed_from_state<I.value, EventId> (signal, e);
        }
      );
    }
    return detail::processed_status;
  }

  // Runtime dispatch using stored event_id - uses dispatch thunks table
  template <typename E>
  detail::dispatch_status_t dispatch_by_event_id(std::size_t event_id, const E &e) noexcept {
    if (event_id >= normalized_model.event_count) {
      return detail::processed_status;
    }
    // Use static_switch on event_id to call the correct typed dispatch
    detail::dispatch_status_t result = detail::processed_status;
    static_switch<0, normalized_model.event_count>(
      event_id,
      [&](auto I) {
        result = this->template dispatch_typed_by_id_core<I.value>(e);
      }
    );
    return result;
  }

  // Runtime deferred enqueue using stored event_id
  template <typename E>
  void enqueue_deferred_runtime(std::size_t event_id, const E &e) noexcept {
    if (event_id >= normalized_model.event_count) {
      return;
    }
    static_switch<0, normalized_model.event_count>(
      event_id,
      [&](auto I) {
        (void) this->template enqueue_deferred<I.value>(e);
      }
    );
  }

  // ISR-safe dispatch: enqueue event, wake engine, return immediately.
  // Processing happens in the engine task, not inline.
  template <std::size_t EventId, typename E>
  detail::dispatch_status_t dispatch_typed_by_id(const E &e) noexcept {
    static_assert(
      EventId != detail::invalid_index,
      "hsm::HSM::dispatch<T>() used with event type T whose kind is not present in the model"
    );

    // Delegate to fire-and-forget enqueue
    detail::dispatch_status_t result = enqueue_typed_by_id<EventId>(e);

    // Wake the engine to process the queued event
    if (result == detail::processed_status) {
      wake_signal_.set();
    }

    return result;
  }

public:

  constexpr void on_activity_complete(std::size_t idx) noexcept {
    if (idx < active_tasks_.size ()) {
      active_tasks_[idx].reset ();
      active_tasks_mask_[idx / 64] &= ~(1ULL << (idx % 64));
    }
    SignalType signal{};
    if (signal_) signal.reset (signal_);
    if constexpr (has_completion) {
      resolve_completion (signal);
    }
  }

  // Template version for compile-time known index (used by new create_task API)
  template <std::size_t Idx>
  constexpr void on_activity_complete_t() noexcept {
    on_activity_complete(Idx);
  }

  template <std::size_t TimerIdx>
  constexpr void dispatch_timer_event() noexcept {
    using E = TimeEvent;
    constexpr std::size_t EventId = timer_event_map_t::template event_id<TimerIdx> ();
    static_assert (EventId != detail::invalid_index,
                   "hsm::HSM::dispatch_timer_event<TimerIdx>() has no associated event id");
    E e{};
    (void) enqueue_typed_by_id<EventId> (e);
  }

private:
  constexpr bool is_deferred(std::size_t state,
                             std::size_t event_id) const noexcept {
    if constexpr (!has_deferred_events) return false;

    if (state >= normalized_model.state_count) return false;
    
    // Direct O(1) bitmask check
    return (normalized_model.transitive_deferred_masks[state][event_id / 64] & (1ULL << (event_id % 64))) != 0;
  }

  // Per-transition guard/effect helpers allow compile-time specialization
  // when the transition id is known statically.
  template <std::size_t TId, typename E>
  constexpr bool eval_guard(SignalType &signal, const E &e) noexcept {
    constexpr auto t = normalized_model.transitions[TId];
    if constexpr (t.guard_idx == detail::invalid_index) {
      return true;
    } else {
      constexpr std::size_t idx = t.guard_idx;
      static_assert (
        idx < std::tuple_size_v<decltype(guard_tuple)>,
        "guard index out of range"
      );
      auto res = invoke_typed (
        std::get<idx> (guard_tuple),
        signal,
        self (),
        e
      );
      if constexpr (std::is_same_v<decltype(res), detail::not_invoked>) {
        return false;
      } else {
        return static_cast<bool> (res);
      }
    }
  }

  template <std::size_t TId, typename E>
  constexpr void run_effects(SignalType &signal, const E &e) noexcept {
    constexpr auto t = normalized_model.transitions[TId];
    if constexpr (t.effect_start != detail::invalid_index) {
      for_each_index<t.effect_start, t.effect_count> (
        [&](auto I) {
          invoke_typed (
            std::get<I> (effect_tuple),
            signal,
            self (),
            e
          );
        }
      );
    }
  }

  // Compile-time path calculation (Step 5)
  // Replaces runtime stack arrays for exit/enter paths.
  template <std::size_t TId>
  struct transition_paths;  // Forward decl

  // Optimized execute_transition that uses transition_paths
  template <std::size_t TId, typename E>
  constexpr void execute_transition_optimized(SignalType &signal,
                                              const E &e) noexcept {
    constexpr auto t = normalized_model.transitions[TId];

    if constexpr (t.history != detail::history_kind::none) {
      std::size_t target = t.target_id;
      std::size_t old_state = current_state_id_;

      [[maybe_unused]] bool used_history_default = false;

      // History resolution
      std::size_t parent = t.history_parent;
      if (parent != detail::invalid_index) {
        std::size_t h_idx = history_map_t::map[parent];
        std::size_t leaf = (h_idx != detail::invalid_index)
                               ? last_active_leaf_[h_idx]
                               : detail::invalid_index;

        if (leaf == detail::invalid_index) {
          // No prior history recorded for this composite. If this transition
          // targets a named history pseudostate with an explicit default
          // transition, consult that default first.
          if constexpr (t.history_state_id != detail::invalid_index) {
            constexpr auto hist_state =
              normalized_model.states[t.history_state_id];
            constexpr std::size_t def_tid =
              hist_state.history_default_transition_id;

            if constexpr (def_tid != detail::invalid_index) {
              // Evaluate default guard (if any).
              if (eval_guard<def_tid>(signal, e)) {
                constexpr auto def_trans =
                  normalized_model.transitions[def_tid];
                target = def_trans.target_id;
                used_history_default = true;
              } else {
                // Guard rejected the default -> fall back to composite initial
                target = parent;
              }
            } else {
              // Named history with no explicit default -> composite initial
              target = parent;
            }
          } else {
            // Legacy history (shallow_history/deep_history wrappers)
            // with no named pseudostate -> composite initial
            target = parent;
          }
        } else if (t.history == detail::history_kind::deep) {
          target = leaf;
        } else {
          std::size_t child = leaf;
          while (child != detail::invalid_index) {
            std::size_t p = normalized_model.states[child].parent_id;
            if (p == parent) break;
            child = p;
          }
          target = (child != detail::invalid_index) ? child : parent;
        }
      } else {
        target = current_state_id_;
      }

      exit_to_lca_runtime (
        signal,
        e,
        old_state,
        target,
        t.type
      );

      // If we selected a named history default, run its effects exactly once
      // for the default case before the history transition's own effects.
      if constexpr (t.history_state_id != detail::invalid_index) {
        constexpr auto hist_state =
          normalized_model.states[t.history_state_id];
        constexpr std::size_t def_tid =
          hist_state.history_default_transition_id;
        if constexpr (def_tid != detail::invalid_index) {
          if (used_history_default) {
            run_effects<def_tid>(signal, e);
          }
        }
      }

      if constexpr (t.effect_start != detail::invalid_index) {
        for_each_index<t.effect_start, t.effect_count> (
          [&](auto I) {
            invoke_typed (
              std::get<I> (effect_tuple),
              signal,
              self (),
              e
            );
          }
        );
      }

      if (target != detail::invalid_index) {
        enter_from_lca_runtime (
          signal,
          e,
          old_state,
          target,
          t.type
        );
        resolve_initial_runtime (
          signal,
          e,
          target
        );
        if constexpr (has_completion) {
          resolve_completion (signal);
        }
        if constexpr (has_history) {
          update_history_from_leaf (current_state_id_);
        }
      }
    } else {
      using paths = transition_paths<TId>;
      using substate_exit = substate_exit_helper<TId>;

      // UML 2.5.1: For group transitions from composite states,
      // exit active substates first (innermost to source)
      substate_exit::exit_substates(*this, signal, e);

      if constexpr (has_any_entry_exit_or_actions) {
        paths::for_each_effective_exit (
          [&](auto StateIdConst) {
            exit_state_impl<StateIdConst.value> (
              signal,
              e
            );
          }
        );
      }
      run_effects<TId> (
        signal,
        e
      );
      if constexpr (has_any_entry_exit_or_actions) {
        paths::for_each_effective_enter (
          [&](auto StateIdConst) {
            enter_state_impl<StateIdConst.value> (
              signal,
              e
            );
          }
        );
      }

      // Direct recursion for initial resolution
      constexpr std::size_t target = t.target_id;
      if constexpr (target != detail::invalid_index) {
        resolve_initial_compile_time<target> (
          signal,
          e
        );
        // resolve_completion is handled at the end of the chain
        if constexpr (has_history) {
          update_history_from_leaf (current_state_id_);
        }
      }
    }
  }

  // Compile-time path calculation (Step 5)
  // Replaces runtime stack arrays for exit/enter paths.
  template <std::size_t TId>
  struct transition_paths {
    // 1. Calculate LCA
    // 2. Build exit path (Source -> LCA)
    // 3. Build enter path (LCA -> Target)

    static constexpr auto trans = normalized_model.transitions[TId];
    static constexpr std::size_t source = trans.source_id;
    static constexpr std::size_t target = trans.target_id;

    // Compute the LCA directly from the normalized_model parent chains at
    // compile time for this transition.

    static consteval std::size_t calculate_lca() {
      if (target == detail::invalid_index) return detail::invalid_index;

      // For external self-transitions
      if (trans.type == detail::transition_kind::external && source == target) {
        if (source == detail::invalid_index) return detail::invalid_index;
        return normalized_model.states[source].parent_id;
      }
      if (trans.type == detail::transition_kind::local && source == target) {
        return source;
      }

      std::size_t s = source;
      std::size_t t = target;

      // Find depths
      std::size_t d_s = 0;
      for (size_t p = s; p != detail::invalid_index;
           p = normalized_model.states[p].parent_id)
        d_s++;
      std::size_t d_t = 0;
      for (size_t p = t; p != detail::invalid_index;
           p = normalized_model.states[p].parent_id)
        d_t++;

      // Align depths
      while (d_s > d_t) {
        s = normalized_model.states[s].parent_id;
        d_s--;
      }
      while (d_t > d_s) {
        t = normalized_model.states[t].parent_id;
        d_t--;
      }

      while (s != t && s != detail::invalid_index &&
             t != detail::invalid_index) {
        s = normalized_model.states[s].parent_id;
        t = normalized_model.states[t].parent_id;
      }
      return s;  // LCA
    }

    static constexpr std::size_t lca = calculate_lca ();

    static consteval auto build_exit() {
      std::array<std::size_t, max_depth + 2> path{};
      std::size_t count = 0;
      if (lca != detail::invalid_index) {
        for (std::size_t s = source; s != lca && s != detail::invalid_index;
             s = normalized_model.states[s].parent_id) {
          path[count++] = s;
        }
      }
      return std::pair{path, count};
    }

    static consteval auto build_enter() {
      // Enter path is LCA -> Target (exclusive LCA, inclusive Target)
      // We build Target -> LCA and reverse
      std::array<std::size_t, max_depth + 2> path{};
      std::size_t count = 0;
      if (lca != detail::invalid_index && target != detail::invalid_index) {
        std::array<std::size_t, max_depth + 2> temp{};
        std::size_t temp_count = 0;
        for (std::size_t s = target; s != lca && s != detail::invalid_index;
             s = normalized_model.states[s].parent_id) {
          temp[temp_count++] = s;
        }
        // Reverse into path
        for (std::size_t i = 0; i < temp_count; ++i) {
          path[count++] = temp[temp_count - 1 - i];
        }
      }
      return std::pair{path, count};
    }

    static constexpr auto exit_info = build_exit ();
    static constexpr auto enter_info = build_enter ();

    static constexpr std::size_t exit_count = exit_info.second;
    static constexpr auto exit_path = exit_info.first;

    static constexpr std::size_t enter_count = enter_info.second;
    static constexpr auto enter_path = enter_info.first;

    static consteval auto build_effective_exit() {
      std::array<std::size_t, max_depth + 2> path{};
      std::size_t count = 0;

      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((state_traits<exit_path[Is]>::has_work_on_exit
              ? (path[count++] = exit_path[Is], void ())
              : void ()),
         ...);
      }(std::make_index_sequence<exit_count>{});

      return std::pair{path, count};
    }

    static consteval auto build_effective_enter() {
      std::array<std::size_t, max_depth + 2> path{};
      std::size_t count = 0;

      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((state_traits<enter_path[Is]>::has_work_on_enter
              ? (path[count++] = enter_path[Is], void ())
              : void ()),
         ...);
      }(std::make_index_sequence<enter_count>{});

      return std::pair{path, count};
    }

    static constexpr auto effective_exit_info = build_effective_exit ();
    static constexpr auto effective_enter_info = build_effective_enter ();

    static constexpr std::size_t effective_exit_count =
      effective_exit_info.second;
    static constexpr auto effective_exit_path = effective_exit_info.first;

    static constexpr std::size_t effective_enter_count =
      effective_enter_info.second;
    static constexpr auto effective_enter_path = effective_enter_info.first;

    template <typename Fun>
    static constexpr void for_each_exit(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(
          std::integral_constant<std::size_t, exit_path[Is]>{}
         ),
         ...);
      }(std::make_index_sequence<exit_count>{});
    }

    template <typename Fun>
    static constexpr void for_each_enter(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(
          std::integral_constant<std::size_t, enter_path[Is]>{}
         ),
         ...);
      }(std::make_index_sequence<enter_count>{});
    }

    template <typename Fun>
    static constexpr void for_each_effective_exit(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(
          std::integral_constant<std::size_t,
                                 effective_exit_path[Is]>{}
         ),
         ...);
      }(std::make_index_sequence<effective_exit_count>{});
    }

    template <typename Fun>
    static constexpr void for_each_effective_enter(Fun &&fun) {
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<Fun> (fun)(
          std::integral_constant<std::size_t,
                                 effective_enter_path[Is]>{}
         ),
         ...);
      }(std::make_index_sequence<effective_enter_count>{});
    }
  };

  // UML 2.5.1 Compliance: Helper for group transitions from composite states.
  // Exits active substates before the main exit path (innermost first).
  template <std::size_t TId>
  struct substate_exit_helper {
    static constexpr auto trans = normalized_model.transitions[TId];
    static constexpr std::size_t source = trans.source_id;

    // Check if source is composite (has initial transition)
    static constexpr bool source_is_composite =
        source != detail::invalid_index &&
        normalized_model.states[source].initial_transition_id != detail::invalid_index;

    // Only external transitions from composite states need substate exit
    static constexpr bool needs_substate_exit =
        source_is_composite &&
        trans.type == detail::transition_kind::external;

    template <typename E>
    static constexpr void exit_substates(HSM &hsm, SignalType &signal,
                                         const E &e) noexcept {
      if constexpr (needs_substate_exit) {
        // Exit from current active state up to (but not including) source.
        // Uses O(1) dispatch per state via exit_state's internal switch.
        for (std::size_t s = hsm.current_state_id_;
             s != source && s != detail::invalid_index;
             s = normalized_model.states[s].parent_id) {
          hsm.exit_state(signal, e, s);
        }
      }
    }
  };

  // Generalized description of the transition chain for a given
  // (StateId, EventId) pair. Currently this is built on top of
  // simple_chain_info and specializes only the single-transition case,
  // but it provides a hook for future multi-step specialization.
  template <std::size_t StateId, std::size_t EventId>
  struct compile_time_chain {
    // Helper to find AnyEvent id
    static consteval std::size_t find_any_event_id() {
      for (std::size_t i = 0; i < normalized_model.event_count; ++i) {
        if (normalized_model.events[i].kind ==
            static_cast<hsm::kind_t> (hsm::Kind::AnyEvent)) {
          return i;
        }
      }
      return detail::invalid_index;
    }

    static constexpr std::size_t any_event_id = find_any_event_id ();

    // Collect transitions from normalized_model
    static consteval auto build() {
      std::array<std::size_t, normalized_model.transition_count> result{};
      std::size_t count = 0;

      std::size_t curr = StateId;
      while (curr != detail::invalid_index) {
        // 1. Specific event transitions in this state
        if (EventId != detail::invalid_index) {
          for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
            const auto &trans = normalized_model.transitions[t];
            if (trans.source_id == curr &&
                trans.event_id != detail::invalid_index) {
              // Check kind match instead of index match
              if (normalized_model.events[trans.event_id].kind ==
                  normalized_model.events[EventId].kind) {
                result[count++] = t;
              }
            }
          }
        }

        // 2. Wildcard transitions in this state (if not already covered?
        // No, hsm matches specific then wildcard in same state)
        if (any_event_id != detail::invalid_index && EventId != any_event_id) {
          for (std::size_t t = 0; t < normalized_model.transition_count; ++t) {
            const auto &trans = normalized_model.transitions[t];
            if (trans.source_id == curr && trans.event_id == any_event_id) {
              result[count++] = t;
            }
          }
        }

        curr = normalized_model.states[curr].parent_id;
      }

      return std::pair{result, count};
    }

    static constexpr auto build_result = build ();
    static constexpr std::size_t length = build_result.second;
    static constexpr auto ids = build_result.first;

    static constexpr bool is_empty = (length == 0);

    static constexpr bool has_guards = []() {
                                         for (std::size_t i = 0; i < length; ++i) {
                                           if (normalized_model.transitions[ids[i]].guard_idx !=
                                               detail::invalid_index)
                                             return true;
                                         }
                                         return false;
                                       }();

    static constexpr bool has_history = []() {
                                          for (std::size_t i = 0; i < length; ++i) {
                                            if (normalized_model.transitions[ids[i]].history !=
                                                detail::history_kind::none)
                                              return true;
                                          }
                                          return false;
                                        }();

    static constexpr bool has_timers = []() {
                                         for (std::size_t i = 0; i < length; ++i) {
                                           if (normalized_model.transitions[ids[i]].timer_type !=
                                               detail::timer_kind::none)
                                             return true;
                                         }
                                         return false;
                                       }();

    template <typename Fun>
    static constexpr bool try_match(Fun &&fun) {
      return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return (std::forward<Fun> (fun)(std::integral_constant<std::size_t, ids[Is]>{}) || ...);
      }(std::make_index_sequence<length>{});
    }
  };

  // Replace transition_chain with compile_time_chain wrapper
  template <std::size_t StateId, std::size_t EventId>
  using transition_chain = compile_time_chain<StateId, EventId>;

  template <std::size_t StateId, std::size_t EventId, typename E>
  constexpr detail::dispatch_status_t dispatch_chain(SignalType &signal, const E &e) noexcept {
    using chain = transition_chain<StateId, EventId>;

    // For typed dispatch with a valid EventId, we always use the
    // compile-time chain. There is no fallback to a separate
    // event-id-based dispatcher.
    if constexpr (chain::length == 0U) {
      // No transitions at all for this (state,event) pair.
      return detail::processed_status;  // Event was processed but no transition matched
    } else {
      // Specialized chain execution with guard checking; iterate
      // through candidates using short-circuiting.
      chain::template try_match (
        [&](auto TIdConstant) {
          constexpr std::size_t t_id = TIdConstant.value;
          if (eval_guard<t_id> (
            signal,
            e
              )) {
            execute_transition_optimized<t_id> (
              signal,
              e
            );
            return true;
          }
          return false;
        }
      );
      return detail::processed_status;
    }
  }

  // Typed-event dispatch specialization. For each (StateId, EventId) we can
  // detect at compile time when there is a single, guardless, non-history
  // transition and call execute_transition directly.
  template <std::size_t StateId, std::size_t EventId, typename E>
  constexpr detail::dispatch_status_t dispatch_typed_from_state(SignalType &signal,
                                           const E &e) noexcept {
    using chain = compile_time_chain<StateId, EventId>;

    // Optimization: single guardless non-history transition - skip iteration
    if constexpr (chain::length == 1 && !chain::has_guards && !chain::has_history) {
      // Direct execution without try_match iteration
      execute_transition_optimized<chain::ids[0]>(signal, e);
      return detail::processed_status;
    } else {
      // Fall back to chain dispatch for complex cases
      return dispatch_chain<StateId, EventId>(signal, e);
    }
  }


  template <typename E>
  constexpr void exit_to_lca(SignalType &signal, const E &e, std::size_t source,
                             std::size_t target, detail::transition_kind kind,
                             std::size_t lca) noexcept {
    if (lca == detail::invalid_index) {
      // Fallback to full runtime calculation
      exit_to_lca_runtime (
        signal,
        e,
        source,
        target,
        kind
      );
      return;
    }

    for (std::size_t s = source; s != lca && s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      exit_state (
        signal,
        e,
        s
      );
    }
  }

  template <typename E>
  constexpr void exit_to_lca_runtime(SignalType &signal, const E &e,
                                     std::size_t source, std::size_t target,
                                     detail::transition_kind kind) noexcept {
    std::array<std::size_t, max_depth + 2> source_path{};
    std::size_t source_len = 0;
    for (std::size_t s = source; s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      source_path[source_len++] = s;
    }

    std::array<std::size_t, max_depth + 2> target_path{};
    std::size_t target_len = 0;
    for (std::size_t s = target; s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      target_path[target_len++] = s;
    }

    std::size_t lca = detail::invalid_index;
    int i = static_cast<int> (source_len) - 1;
    int j = static_cast<int> (target_len) - 1;
    while (i >= 0 && j >= 0 &&
           source_path[static_cast<std::size_t> (i)] ==
           target_path[static_cast<std::size_t> (j)]) {
      lca = source_path[static_cast<std::size_t> (i)];
      i--;
      j--;
    }

    if (kind == detail::transition_kind::external && source == target) {
      if (normalized_model.states[source].parent_id != detail::invalid_index) {
        lca = normalized_model.states[source].parent_id;
      } else {
        lca = detail::invalid_index;
      }
    }

    for (std::size_t s = source; s != lca && s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      exit_state (
        signal,
        e,
        s
      );
    }
  }

  template <typename E>
  constexpr void enter_from_lca(SignalType &signal, const E &e,
                                std::size_t source, std::size_t target,
                                detail::transition_kind kind,
                                std::size_t lca) noexcept {
    if (lca == detail::invalid_index) {
      enter_from_lca_runtime (
        signal,
        e,
        source,
        target,
        kind
      );
      return;
    }

    std::array<std::size_t, max_depth + 2> target_path{};
    std::size_t target_len = 0;

    // Build path from target up to LCA
    for (std::size_t s = target; s != lca && s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      target_path[target_len++] = s;
    }

    // Enter in reverse (from LCA down to target)
    for (int j = static_cast<int> (target_len) - 1; j >= 0; j--) {
      enter_state (
        signal,
        e,
        target_path[static_cast<std::size_t> (j)]
      );
    }
  }

  template <typename E>
  constexpr void enter_from_lca_runtime(SignalType &signal, const E &e,
                                        std::size_t source, std::size_t target,
                                        detail::transition_kind kind) noexcept {
    std::array<std::size_t, max_depth + 2> source_path{};
    std::size_t source_len = 0;
    for (std::size_t s = source; s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      source_path[source_len++] = s;
    }

    std::array<std::size_t, max_depth + 2> target_path{};
    std::size_t target_len = 0;
    for (std::size_t s = target; s != detail::invalid_index;
         s = normalized_model.states[s].parent_id) {
      target_path[target_len++] = s;
    }

    int i = static_cast<int> (source_len) - 1;
    int j = static_cast<int> (target_len) - 1;
    while (i >= 0 && j >= 0 &&
           source_path[static_cast<std::size_t> (i)] ==
           target_path[static_cast<std::size_t> (j)]) {
      i--;
      j--;
    }

    if (kind == detail::transition_kind::external && source == target) {
      j = 0;
    }

    for (; j >= 0; j--) {
      enter_state (
        signal,
        e,
        target_path[static_cast<std::size_t> (j)]
      );
    }
  }

  // State-specialized exit implementation
  template <std::size_t StateId, typename E>
  constexpr void exit_state_impl(SignalType &signal, const E &e) noexcept {
    if constexpr (!has_any_entry_exit_or_actions) {
      (void) signal;
      (void) e;
      return;
    }

    constexpr auto s = normalized_model.states[StateId];

    if constexpr (state_traits<StateId>::has_exit) {
      for_each_index<s.exit_start, s.exit_count> (
        [&](auto I) {
          invoke_typed (
            std::get<I> (exit_tuple),
            signal,
            self (),
            e
          );
        }
      );
    }

    if constexpr (state_traits<StateId>::has_timers) {
      state_timers<StateId>::for_each (
        [&](auto TIdConst) {
          constexpr std::size_t t_id = TIdConst.value;
          constexpr auto &trans = normalized_model.transitions[t_id];
          if constexpr (requires { this->cancel_timer (trans.timer_idx); }) {
            this->cancel_timer (trans.timer_idx);
          }
          if (trans.timer_idx < active_timer_tasks_.size () &&
              active_timer_tasks_[trans.timer_idx].has_value ()) {
            // Set signal to cancel the timer. Don't reset the task here because
            // if the timer is the one that triggered this state exit (timer
            // fired and called dispatch_timer_event), we're still running
            // inside that coroutine and destroying the task would destroy the
            // coroutine frame while it's executing.
            // The task will be cleaned up when:
            // 1. A new timer starts in this slot (it will replace the old one)
            // 2. The HSM is destroyed (destructor handles cleanup)
            active_timer_tasks_[trans.timer_idx]->signal->set ();
            // DON'T reset here - leave the task alive until coroutine completes
            // active_timer_tasks_[trans.timer_idx].reset ();
            // active_timer_tasks_mask_[trans.timer_idx / 64] &= ~(1ULL << (trans.timer_idx % 64));
          }
        }
      );
    }

    if constexpr (state_traits<StateId>::has_activities) {
      for_each_index<s.activity_start, s.activity_count> (
        [&](auto I) {
          constexpr std::size_t idx = I;
          if (idx < active_tasks_.size () && active_tasks_[idx].has_value ()) {
            active_tasks_[idx]->signal->set ();
            if (active_tasks_[idx]->task.joinable ()) {
              active_tasks_[idx]->task.join ();
            }
            active_tasks_[idx].reset ();
            active_tasks_mask_[idx / 64] &= ~(1ULL << (idx % 64));
          }
        }
      );
    }
  }

  // State-specialized enter implementation
  template <std::size_t StateId, typename E>
  constexpr void enter_state_impl(SignalType &signal, const E &e) noexcept {
    if constexpr (!has_any_entry_exit_or_actions) {
      (void) signal;
      (void) e;
      return;
    }

    constexpr auto s = normalized_model.states[StateId];

    if constexpr (state_traits<StateId>::has_entry) {
      for_each_index<s.entry_start, s.entry_count> (
        [&](auto I) {
          invoke_typed (
            std::get<I> (entry_tuple),
            signal,
            self (),
            e
          );
        }
      );
    }

    if constexpr (state_traits<StateId>::has_activities) {
      for_each_index<s.activity_start, s.activity_count> (
        [&](auto I) {
          constexpr std::size_t idx = I;
          if (idx < active_tasks_.size ()) {
            activity_contexts_[idx].reset (signal_);
            SignalType* activity_signal = &activity_contexts_[idx];

            // Create ActivityTask based on activity return type
            using ActivityCallable = std::tuple_element_t<idx, decltype(activity_tuple)>;
            using ResultType = decltype(invoke_typed(
              std::declval<ActivityCallable&>(),
              std::declval<SignalType&>(),
              std::declval<instance_type&>(),
              std::declval<const E&>()
            ));

            if constexpr (std::is_same_v<ResultType, ActivityTask>) {
              // Activity returns ActivityTask - use as coroutine
              ActivityTask task = invoke_typed(
                std::get<idx>(activity_tuple),
                *activity_signal,
                this->self(),
                e
              );

              // Set completion callback
              task.on_done([this]() {
                on_activity_complete(idx);
              });

              // Store the task BEFORE starting (callback may fire during start)
              active_tasks_[idx].emplace(ActiveTask{std::move(task), activity_signal});
              active_tasks_mask_[idx / 64] |= (1ULL << (idx % 64));

              // Start the coroutine (may complete synchronously and fire callback)
              if (active_tasks_[idx]) {
                active_tasks_[idx]->task.start();
              }
            } else {
              // Activity returns void - execute immediately and fire completion
              invoke_typed(
                std::get<idx>(activity_tuple),
                *activity_signal,
                this->self(),
                e
              );
              // Fire completion immediately for synchronous activities
              on_activity_complete(idx);
            }
          }
        }
      );
    }

    if constexpr (state_traits<StateId>::has_timers) {
      state_timers<StateId>::for_each (
        [&](auto TIdConst) {
          constexpr std::size_t t_id = TIdConst.value;
          constexpr auto& trans = normalized_model.transitions[t_id];

          if (trans.timer_idx < std::tuple_size_v<decltype(timer_tuple)>) {
            timer_contexts_[trans.timer_idx].reset(signal_);
            SignalType* timer_signal = &timer_contexts_[trans.timer_idx];

            // Create timer coroutine (returns TaskType for deadline tracking)
            TaskType task = timer_coro<trans.timer_idx, trans.timer_type>(
              *coro_pool_,
              *timer_signal,
              e,
              *this
            );

            task.start();

            active_timer_tasks_[trans.timer_idx].emplace(
              TimerActiveTask{std::move(task), timer_signal}
            );
            active_timer_tasks_mask_[trans.timer_idx / 64] |= (1ULL << (trans.timer_idx % 64));
          }
        }
      );
    }
  }

  template <std::size_t... Is, typename E>
  constexpr void dispatch_exit_state(SignalType &signal, const E &e,
                                     std::size_t s_id,
                                     std::index_sequence<Is...>) noexcept {
    (void)(((s_id == Is && (exit_state_impl<Is> (
      signal,
      e
                            ), true)) || ...));
  }

  template <std::size_t... Is, typename E>
  constexpr void dispatch_enter_state(SignalType &signal, const E &e,
                                      std::size_t s_id,
                                      std::index_sequence<Is...>) noexcept {
    (void)(((s_id == Is && (enter_state_impl<Is> (
      signal,
      e
                            ), true)) || ...));
  }

  template <typename E>
  constexpr void exit_state(SignalType &signal, const E &e,
                            std::size_t s_id) noexcept {
    if (s_id == detail::invalid_index) return;
    dispatch_exit_state (
      signal,
      e,
      s_id,
      std::make_index_sequence<decltype(normalized_model)::state_count>{}
    );
  }

  template <typename E>
  constexpr void enter_state(SignalType &signal, const E &e,
                             std::size_t s_id) noexcept {
    if (s_id == detail::invalid_index) return;
    dispatch_enter_state (
      signal,
      e,
      s_id,
      std::make_index_sequence<decltype(normalized_model)::state_count>{}
    );
  }

  // Recursive compile-time initial resolution
  template <std::size_t StateId, typename E>
  constexpr void resolve_initial_compile_time(SignalType &signal,
                                              const E &e) noexcept {
    current_state_id_ = StateId;

    constexpr auto init_t_id =
      normalized_model.states[StateId].initial_transition_id;

    if constexpr (init_t_id != detail::invalid_index) {
      // Has initial transition -> execute it
      constexpr auto &t = normalized_model.transitions[init_t_id];

      // 1. Execute effects
      if constexpr (t.effect_start != detail::invalid_index) {
        for_each_index<t.effect_start, t.effect_count> (
          [&](auto I) {
            invoke_typed (
              std::get<I> (effect_tuple),
              signal,
              self (),
              e
            );
          }
        );
      }

      // 2. Enter target
      constexpr std::size_t target = t.target_id;
      if constexpr (target != detail::invalid_index) {
        // Note: Initial transitions are usually local/internal within the
        // parent. We must enter from StateId down to Target. The
        // transition_paths<init_t_id> should handle this if calculate_lca works
        // for local. For initial transition from P->C, Source=P, Target=C.
        // LCA=P. Enter=C.
        if constexpr (has_any_entry_exit_or_actions) {
          using paths = transition_paths<init_t_id>;
          paths::for_each_effective_enter (
            [&](auto StateIdConst) {
              enter_state_impl<StateIdConst.value> (
                signal,
                e
              );
            }
          );
        }

        // 3. Recurse
        resolve_initial_compile_time<target> (
          signal,
          e
        );
      }
    } else {
      // No initial transition -> we are at a leaf (or stable state)
      // Run completion logic directly
      if constexpr (has_completion) {
        resolve_completion_step<StateId> (signal);
      }
    }
  }

  template <std::size_t StateId>
  constexpr void resolve_completion_step(SignalType &signal) noexcept {
    // 1. Check activities
    constexpr auto s = normalized_model.states[StateId];
    if constexpr (state_traits<StateId>::has_activities) {
      for_each_index<s.activity_start, s.activity_count> (
        [&](auto I) {
          constexpr std::size_t idx = I;
          if (idx < active_tasks_.size () && active_tasks_[idx].has_value ()) {
            // Activity running
            return;
          }
        }
      );
      // Wait, I can't return from lambda to stop the function.
      // I need a flag.
    }

    // Re-check activities with explicit loop/flag for runtime check
    if constexpr (state_traits<StateId>::has_activities) {
      bool busy = false;
      for (std::size_t i = 0; i < s.activity_count; ++i) {
        std::size_t idx = s.activity_start + i;
        if (idx < active_tasks_.size () && active_tasks_[idx].has_value ()) {
          busy = true;
          break;
        }
      }
      if (busy) return;
    }

    // 2. Check completion transitions
    bool handled = false;
    if constexpr (completion_chain<StateId>::count > 0) {
      completion_chain<StateId>::for_each (
        [&](auto TIdConst) {
          if (handled) return;
          constexpr std::size_t t_id = TIdConst.value;
          constexpr auto &t = normalized_model.transitions[t_id];

          bool guard_passed = true;
      if constexpr (t.guard_idx != detail::invalid_index) {
            CompletionEvent empty{};
            auto res = invoke_typed (
              std::get<t.guard_idx> (guard_tuple),
              signal,
              self (),
              empty
            );
            if constexpr (std::is_same_v<decltype(res), detail::not_invoked>) {
              guard_passed = false;
            } else {
              guard_passed = static_cast<bool> (res);
            }
          }

          if (guard_passed) {
            CompletionEvent empty{};
            execute_transition_optimized<t_id> (
              signal,
              empty
            );
            handled = true;
          }
        }
      );
    }

    if (handled) return;

    // 3. Recurse up if Final
    // Optimized: Direct call to parent completion instead of table
    if constexpr ((s.flags & detail::state_flags::final) !=
                  detail::state_flags::none) {
      constexpr std::size_t parent = s.parent_id;
      if constexpr (parent != detail::invalid_index) {
        resolve_completion_step<parent> (signal);
      }
    }
  }

  constexpr void resolve_completion(SignalType &signal) noexcept {
    if constexpr (!has_completion) {
      (void) signal;
      return;
    }

    if (current_state_id_ == detail::invalid_index) return;

    if (current_state_id_ < normalized_model.state_count) {
      static_switch<0, normalized_model.state_count> (
        current_state_id_,
        [&](auto I) {
          this->resolve_completion_step<I.value> (signal);
        }
      );
    }
  }

  // Update history arrays from the currently active leaf state.
  constexpr void update_history_from_leaf(std::size_t leaf_id) noexcept {
    if constexpr (!has_history) {
      (void) leaf_id;
      return;
    }

    if (leaf_id == detail::invalid_index) return;
    std::size_t curr = leaf_id;
    while (curr != detail::invalid_index) {
      std::size_t h_idx = history_map_t::map[curr];
      if (h_idx != detail::invalid_index) {
        last_active_leaf_[h_idx] = leaf_id;
      }
      curr = normalized_model.states[curr].parent_id;
    }
  }

  // Dispatch Tables (Static Storage for C++20 compliance)
  // Defined at end of class to ensure member function visibility

  template <std::size_t EventId, typename E>
  struct TypedDispatchTable {
    static constexpr auto table = []() {
                                    std::array<bool (HSM::*)(SignalType &, const E &),
                                               normalized_model.state_count>
                                    arr{};
                                    for_each_index<0, normalized_model.state_count> (
                                      [&](auto I) {
          arr[I.value] =
            &HSM::dispatch_typed_from_state<I.value, EventId, E>;
        }
                                    );
                                    return arr;
                                  }();
  };

  template <typename E>
  constexpr void resolve_initial_runtime(SignalType &signal, const E &e,
                                         std::size_t s_id) noexcept {
    if (s_id != detail::invalid_index) {
      current_state_id_ = s_id;
      if (s_id < normalized_model.state_count) {
        // Dispatch to the compile-time resolution via a table
        static_switch<0, normalized_model.state_count> (
          s_id,
          [&](auto I) {
            this->resolve_initial_compile_time<I.value> (signal, e);
          }
        );
      }
    }
  }

};

template <typename Context, typename Machine>
[[nodiscard]] auto TakeSnapshot(Context &ctx, const Machine &machine)
    noexcept(noexcept(machine.takeSnapshot()))
    -> decltype(machine.takeSnapshot()) {
  (void) ctx;
  return machine.takeSnapshot();
}

template <typename Context, typename Machine>
[[nodiscard]] auto TakeSnapshot(const Context &ctx, const Machine &machine)
    noexcept(noexcept(machine.takeSnapshot()))
    -> decltype(machine.takeSnapshot()) {
  (void) ctx;
  return machine.takeSnapshot();
}

template <typename Machine>
[[nodiscard]] auto TakeSnapshot(const Machine &machine)
    noexcept(noexcept(machine.takeSnapshot()))
    -> decltype(machine.takeSnapshot()) {
  return machine.takeSnapshot();
}

template <typename Context, typename Machine>
[[nodiscard]] auto take_snapshot(Context &ctx, const Machine &machine)
    noexcept(noexcept(TakeSnapshot(ctx, machine)))
    -> decltype(TakeSnapshot(ctx, machine)) {
  return TakeSnapshot(ctx, machine);
}

template <typename Context, typename Machine>
[[nodiscard]] auto take_snapshot(const Context &ctx, const Machine &machine)
    noexcept(noexcept(TakeSnapshot(ctx, machine)))
    -> decltype(TakeSnapshot(ctx, machine)) {
  return TakeSnapshot(ctx, machine);
}

template <typename Machine>
[[nodiscard]] auto take_snapshot(const Machine &machine)
    noexcept(noexcept(TakeSnapshot(machine)))
    -> decltype(TakeSnapshot(machine)) {
  return TakeSnapshot(machine);
}
} // namespace hsm

#endif // HSM_HSM_HPP
