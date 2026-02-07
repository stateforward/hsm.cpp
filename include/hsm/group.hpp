#ifndef HSM_GROUP_HPP
#define HSM_GROUP_HPP

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

#include "hsm.hpp"

namespace hsm {

// Forward declaration
template <typename... Machines>
class Group;

// Trait to detect Group
template<typename T> struct is_group_t : std::false_type {};

template<typename... Ms> struct is_group_t<Group<Ms...>> : std::true_type {};

template<typename T> inline constexpr bool is_group_v = is_group_t<T>::value;

template <typename T>
struct leaf_extractor {
    static auto get(T& t) {
        return std::make_tuple(&t);
    }
};

template <typename... Ms>
struct leaf_extractor<Group<Ms...>> {
    static auto get(Group<Ms...>& g) {
        return g.leaves();
    }
};

// GroupTask: aggregate driver for multiple Task-like objects.
// Mirrors Task API while iterating over all tasks.
template <typename... Tasks>
class GroupTask {
public:
    static_assert(sizeof...(Tasks) > 0, "hsm::GroupTask requires at least one task.");
    using deadline_type = decltype(std::declval<std::tuple_element_t<0, std::tuple<Tasks...>>&>().deadline());
    static_assert((std::is_same_v<deadline_type, decltype(std::declval<Tasks&>().deadline())> && ...),
                  "hsm::GroupTask requires all tasks to share the same deadline() type.");

    explicit GroupTask(Tasks&&... tasks)
        : tasks_(std::forward<Tasks>(tasks)...) {}

    explicit GroupTask(std::tuple<Tasks...>&& tasks)
        : tasks_(std::move(tasks)) {}

    // Check if all tasks have completed.
    [[nodiscard]] bool done() const noexcept {
        bool all_done = true;
        std::apply([&](auto const&... t) {
            ((all_done = all_done && t.done()), ...);
        }, tasks_);
        return all_done;
    }

    // Check if any task has work remaining.
    [[nodiscard]] bool joinable() const noexcept {
        bool any_joinable = false;
        std::apply([&](auto const&... t) {
            ((any_joinable = any_joinable || t.joinable()), ...);
        }, tasks_);
        return any_joinable;
    }

    // Resume all tasks once. Returns true if any task still has work remaining.
    bool resume() {
        bool any_remaining = false;
        std::apply([&](auto&... t) {
            ((any_remaining = t.resume() || any_remaining), ...);
        }, tasks_);
        maybe_notify_done();
        return any_remaining;
    }

    // Blocking await - resumes all tasks until all are done.
    void await() {
        while (!done()) {
            resume();
        }
        maybe_notify_done();
    }

    // Start all tasks (first resume if needed).
    void start() {
        std::apply([](auto&... t) {
            (t.start(), ...);
        }, tasks_);
    }

    // Get the earliest deadline among tasks (if any).
    [[nodiscard]] deadline_type deadline() const noexcept {
        deadline_type earliest{};
        std::apply([&](auto const&... t) {
            (update_deadline(earliest, t), ...);
        }, tasks_);
        return earliest;
    }

    // Set completion callback (fires once when all tasks are done).
    void on_done(std::function<void()> callback) {
        on_done_callback_ = std::move(callback);
        maybe_notify_done();
    }

    // Awaitable interface - allows co_await on GroupTask.
    bool await_ready() const noexcept { return done(); }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept {
        continuation_ = awaiting;
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}

private:
    template <typename TaskT>
    static void update_deadline(deadline_type& earliest, const TaskT& task) {
        if (task.done()) return;
        auto d = task.deadline();
        if (!d) return;
        if (!earliest || *d < *earliest) {
            earliest = d;
        }
    }

    void maybe_notify_done() {
        if (!done_notified_ && done()) {
            done_notified_ = true;
            if (on_done_callback_) {
                on_done_callback_();
            }
            if (continuation_) {
                auto cont = continuation_;
                continuation_ = nullptr;
                cont.resume();
            }
        }
    }

    std::tuple<Tasks...> tasks_;
    std::function<void()> on_done_callback_{};
    bool done_notified_{false};
    std::coroutine_handle<> continuation_{nullptr};
};

template <typename... Tasks>
GroupTask(Tasks&&...) -> GroupTask<std::decay_t<Tasks>...>;

template <typename... Machines>
class Group final : public Instance {
public:
    // Constructor takes references to Machines
    constexpr Group(Machines&... items)
        : machines_(&items...),
          leaves_(std::tuple_cat(leaf_extractor<Machines>::get(items)...)) {}

    // Constructor with ID
    constexpr Group(std::string_view id, Machines&... items)
        : machines_(&items...),
          leaves_(std::tuple_cat(leaf_extractor<Machines>::get(items)...)),
          id_(id) {}

    // Accessor for flattening
    constexpr auto leaves() const {
        return leaves_;
    }

    // ID Accessors
    [[nodiscard]] constexpr std::string_view id() const noexcept override { return id_; }

    // Dispatcher interface - returns result_t like HSM
    result_t dispatch(const EventBase &e) override {
        bool any_handled = false;
        std::apply([&](auto*... m) {
            ((m && m->dispatch(e) != QueueFull ? (any_handled = true) : false), ...);
        }, leaves_);
        return any_handled ? Processed : QueueFull;
    }

    std::string_view state() const noexcept override {
        // Groups aggregate multiple machines and do not expose a single
        // composite state path.
        return {};
    }

    // Start all machines in the group.
    // Returns a GroupTask that can drive all machines together.
    // Use .await() to block until all machines are done.
    auto start() noexcept {
        return std::apply([](auto*... m) {
            return GroupTask{m->start()...};
        }, leaves_);
    }

    // Start all machines with an external cancellation signal.
    // Returns a GroupTask that can drive all machines together.
    template <typename SignalType>
    auto start(SignalType& signal) noexcept {
        return std::apply([&signal](auto*... m) {
            return GroupTask{m->start(signal)...};
        }, leaves_);
    }

    // Check if all machines have been started.
    [[nodiscard]] bool started() const noexcept {
        bool all_started = true;
        std::apply([&all_started](auto*... m) {
            ((all_started = all_started && m->started()), ...);
        }, leaves_);
        return all_started;
    }

    // Dispatch to all (Broadcast) - FLATTENED O(1) call depth
    // Awaits each child dispatch synchronously and aggregates results.
    template <typename E>
    result_t dispatch(const E& e) {
        bool any_handled = false;
        std::apply([&](auto*... m) {
            (dispatch_one(*m, e, any_handled), ...);
        }, leaves_);
        return any_handled ? Processed : QueueFull;
    }

    template <detail::fixed_string Name>
    result_t dispatch() {
        bool any_handled = false;
        std::apply([&](auto*... m) {
            ((m->template dispatch<Name>() != QueueFull ? (any_handled = true) : false), ...);
        }, leaves_);
        return any_handled ? Processed : QueueFull;
    }

    // Send (specific machine by ID, recursive)
    template <typename E>
    result_t dispatch(std::string_view id, const E& e) {
        bool handled = false;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            auto try_send = [&](auto I) {
                if (handled) return;
                auto* child = std::get<I.value>(machines_);
                using ChildType = typename std::remove_pointer_t<decltype(child)>;

                // 1. Direct ID Match
                if (child->id() == id) {
                    if (child->dispatch(e) != QueueFull) handled = true;
                    return;
                }

                // 2. Recursive Search
                if constexpr (is_group_v<ChildType>) {
                    if (child->dispatch(id, e) != QueueFull) handled = true;
                }
            };
            (try_send(std::integral_constant<std::size_t, Is>{}), ...);
        }(std::make_index_sequence<sizeof...(Machines)>{});
        return handled ? Processed : QueueFull;
    }

    template <detail::fixed_string Name>
    result_t dispatch(std::string_view id) {
        bool handled = false;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            auto try_send = [&](auto I) {
                if (handled) return;
                auto* child = std::get<I.value>(machines_);
                using ChildType = typename std::remove_pointer_t<decltype(child)>;

                // 1. Direct ID Match
                if (child->id() == id) {
                    if (child->template dispatch<Name>() != QueueFull) handled = true;
                    return;
                }

                // 2. Recursive Search
                if constexpr (is_group_v<ChildType>) {
                    if (child->template dispatch<Name>(id) != QueueFull) handled = true;
                }
            };
            (try_send(std::integral_constant<std::size_t, Is>{}), ...);
        }(std::make_index_sequence<sizeof...(Machines)>{});
        return handled ? Processed : QueueFull;
    }

private:
    template <typename M, typename E>
    static void dispatch_one(M& m, const E& e, bool& any_handled) {
        if constexpr (requires { M::template supports_event<E>(); }) {
            if constexpr (M::template supports_event<E>()) {
                if (m.dispatch(e) != QueueFull) any_handled = true;
            } else {
                (void)e;
            }
        } else {
            if (m.dispatch(e) != QueueFull) any_handled = true;
        }
    }

    std::tuple<Machines*...> machines_;
    decltype(std::tuple_cat(leaf_extractor<Machines>::get(std::declval<Machines&>())...)) leaves_;
    std::string_view id_{""};
};

// Deduction guide / Factory

template <typename... Machines>
constexpr auto make_group(std::string_view id, Machines&... items) {
    return Group<Machines...>(id, items...);
}

template <typename First, typename... Rest>
requires (!std::is_convertible_v<First, std::string_view>)
constexpr auto make_group(First& first, Rest&... rest) {
    return Group<First, Rest...>(first, rest...);
}

} // namespace hsm

#endif // HSM_GROUP_HPP
