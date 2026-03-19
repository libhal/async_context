// Copyright 2024 - 2026 Khalil Estell and the libhal contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

#include <cstddef>

#include <chrono>
#include <concepts>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <utility>

export module async_context.schedulers;

export import async_context;

namespace async::inline v0 {
/**
 * @brief Concept for an instance-based clock.
 *
 * Deliberately does NOT require static now() — unlike std::chrono clock types
 * — so that hardware clocks can be injected as runtime objects. Any
 * std::chrono clock can be adapted via chrono_clock_adapter.
 *
 * A conforming type must provide:
 *   - time_point  : the type returned by now()
 *   - duration    : the difference type of two time_points
 *   - now()       : const member returning time_point
 *   - time_point arithmetic (subtraction -> duration, addition of duration)
 *   - time_point::max() sentinel for "never"
 */
export template<typename T>
concept clock = requires(T const t, typename T::time_point tp) {
  typename T::time_point;
  typename T::duration;
  { t.now() } -> std::same_as<typename T::time_point>;
  { tp - tp } -> std::convertible_to<typename T::duration>;
  { tp + typename T::duration{} } -> std::same_as<typename T::time_point>;
  { T::time_point::max() } -> std::same_as<typename T::time_point>;
};

/**
 * @brief Adapts any std::chrono-conforming clock (static now()) into an
 * async::clock (instance now()).
 *
 * Zero-size type — all state lives in the underlying static clock. The
 * instance now() simply forwards, so this is always optimized away entirely.
 *
 * Example:
 *   async::chrono_clock_adapter<std::chrono::steady_clock> clk;
 *   static_assert(async::clock<decltype(clk)>);
 */
export template<typename ChronoClock>
  requires requires {
    { ChronoClock::now() } -> std::same_as<typename ChronoClock::time_point>;
    typename ChronoClock::duration;
  }
struct chrono_clock_adapter
{
  using duration = typename ChronoClock::duration;
  using time_point = typename ChronoClock::time_point;

  [[nodiscard]] time_point now() const noexcept(noexcept(ChronoClock::now()))
  {
    return ChronoClock::now();
  }
};

// =============================================================================

/**
 * @brief Internal scheduler entry binding one context to the run_until_done
 * loop.
 *
 * Associates a context reference with an absolute wake deadline and owns the
 * unblock listener registration on that context. On construction (via assign())
 * the listener is registered; on destruction it is unconditionally cleared,
 * ensuring the context never holds a dangling listener pointer after the
 * enclosing run_until_done_impl stack frame is gone.
 *
 * @note Not copyable or movable — the array of scheduled_context objects is
 * stack-allocated inside run_until_done_impl and never escapes that scope.
 *
 * @tparam Clock Any type satisfying async::clock.
 */
template<clock Clock>
struct scheduled_context
{
  using time_point = typename Clock::time_point;

  std::reference_wrapper<context> ctx;
  time_point wake_time = time_point::max();

  scheduled_context(async::context& p_ctx,
                    async::unblock_listener* p_listener,
                    Clock const& p_clock)
    : ctx(p_ctx)
  {
    p_ctx.on_unblock(p_listener);
    refresh(p_clock);
  }

  // Bind this entry to p_ctx, register p_listener (may be nullptr), and
  // compute the initial wake_time. Must be called exactly once before any
  // other method.
  void assign(async::context& p_ctx,
              async::unblock_listener* p_listener,
              Clock const& p_clock)
  {
    ctx = p_ctx;
    p_ctx.on_unblock(p_listener);
    refresh(p_clock);
  }

  // Recompute wake_time from the context's current sleep_time. Called after
  // every resume because the blocking state may have changed.
  void refresh(Clock const& p_clock)
  {
    if (ctx.get().state() == blocked_by::time) {
      wake_time = p_clock.now() + ctx.get().sleep_time();
    } else {
      wake_time = time_point::max();
    }
  }

  /**
   * @brief Resume the context if its deadline has elapsed or it is otherwise
   * ready, then refresh wake_time to reflect the new state.
   *
   * @param p_clock - clock to get the current time from
   * @return true - context was resumed
   * @return false - context is done
   */
  void resume(Clock const& p_clock)
  {
    if (ctx.get().state() == blocked_by::time && wake_time <= p_clock.now()) {
      // Deadline elapsed — unblock without triggering scheduler notification,
      // since we are the scheduler, then resume.
      ctx.get().unblock_without_notification();
      ctx.get().resume();
      // Recompute after resume — state may have changed.
      refresh(p_clock);
    } else if (is_ready()) {
      ctx.get().resume();
      // Recompute after resume — state may have changed.
      refresh(p_clock);
    }
  }

  // Returns true if the context is not blocked by time or I/O and can be
  // resumed immediately.
  [[nodiscard]] bool is_ready() const noexcept
  {
    return ctx.get().state() != blocked_by::io &&
           ctx.get().state() != blocked_by::time;
  }

  [[nodiscard]] bool operator<(time_point const& p_other) const noexcept
  {
    return wake_time < p_other;
  }

  // Clear the unblock listener so the context does not hold a pointer into
  // this stack frame after run_until_done_impl returns.
  ~scheduled_context()
  {
    ctx.get().clear_unblock_listener();
  }
};

/**
 * @brief Concept for types derived from async::context
 *
 * This concept is satisfied by async::context and any type derived from it,
 * such as proxy_context and inplace_context. It is used to constrain the
 * variadic context parameters in run_until_done to ensure type safety.
 *
 * @tparam T The type to check
 */
export template<class T>
concept context_derived = std::derived_from<T, context>;

// Internal implementation — accepts a nullable listener pointer.
// nullptr means no listener is registered (no interruptable sleep).
template<clock Clock>
void run_until_done_impl(
  Clock& p_clock,
  std::invocable<typename Clock::time_point> auto&& p_sleep_until,
  async::unblock_listener* p_listener,
  context_derived auto&... p_tasks)
{
  using time_point = typename Clock::time_point;
  constexpr auto context_count = sizeof...(p_tasks);
  constexpr auto max_wake = time_point::max();

  // Stack-allocated scheduler table. Each entry holds a reference to its
  // context and manages the unblock listener registration on that context.
  // Because the array is destroyed when this function returns — whether by
  // normal exit or exception — every scheduled_context destructor fires before
  // the stack frame unwinds, guaranteeing that all listener pointers are
  // cleared before p_listener (or p_sleep_until's internal primitive) could
  // go out of scope. This makes listener lifetime management exception-safe
  // with no additional cleanup code required.
  std::array<scheduled_context<Clock>, context_count> tasks{
    scheduled_context<Clock>{ p_tasks, p_listener, p_clock }...
  };

  while (true) {
    time_point soonest_wake_time = max_wake;
    bool all_done = true;
    bool can_sleep = true;

    for (auto& task : tasks) {
      if (task.ctx.get().done()) {
        continue;
      }

      all_done = false;

      task.resume(p_clock);

      // After resuming, re-evaluate whether we can sleep and what the
      // soonest deadline is
      if (task.wake_time < soonest_wake_time) {
        soonest_wake_time = task.wake_time;
      }

      if (task.is_ready()) {
        can_sleep = false;
      }
    }

    if (all_done) {
      break;
    }

    if (can_sleep && soonest_wake_time < max_wake) {
      p_sleep_until(soonest_wake_time);
    }
  }
}

/**
 * @brief Drives a set of async::context objects to completion.
 *
 * Runs a cooperative scheduling loop, resuming each context that is ready and
 * sleeping (via p_sleep_until) when all contexts are either blocked by time or
 * blocked by I/O.
 *
 * Exceptions that propagate out of the coroutine will be propagated out of this
 * function.
 *
 * @tparam Clock - any type satisfying async::clock
 *
 * @param p_clock - clock used to compute and compare deadlines
 * @param p_sleep_until - callable(Clock::time_point) that sleeps until the
 * given absolute deadline. May return early if woken externally.
 * @param p_tasks - list of contexts to drive.
 */
export template<clock Clock>
void run_until_done(
  Clock& p_clock,
  std::invocable<typename Clock::time_point> auto&& p_sleep_until,
  context_derived auto&... p_tasks)
{
  run_until_done_impl(p_clock,
                      std::forward<decltype(p_sleep_until)>(p_sleep_until),
                      nullptr,
                      p_tasks...);
}

/**
 * @brief Drives a set of async::context objects to completion with an
 * interruptable sleep.
 *
 * Same as the two-parameter overload but registers p_listener on every context
 * so that an I/O completion or ISR can wake the sleep function early.
 *
 * Exceptions that propagate out of the coroutine will be propagated out of this
 * function.
 *
 * @tparam Clock - any type satisfying async::clock
 *
 * @param p_clock - clock used to compute and compare deadlines
 * @param p_sleep_until - callable(Clock::time_point) that sleeps until the
 * given absolute deadline or until woken via p_listener.
 * @param p_listener - unblock listener wired to the sleep primitive so it
 * returns early when an I/O operation completes.
 * @param p_tasks - list of contexts to drive.
 */
export template<clock Clock>
void run_until_done(
  Clock& p_clock,
  std::invocable<typename Clock::time_point> auto&& p_sleep_until,
  async::unblock_listener&& p_listener,
  context_derived auto&... p_tasks)
{
  run_until_done_impl(p_clock,
                      std::forward<decltype(p_sleep_until)>(p_sleep_until),
                      &p_listener,
                      p_tasks...);
}
}  // namespace async::inline v0
