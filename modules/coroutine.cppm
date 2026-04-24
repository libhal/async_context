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
#include <cstdint>

#include <bit>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <exception>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

export module async_context:coroutine;

namespace async::inline v0 {
/**
 * @brief The element type for async context stack memory buffers.
 *
 * Stack buffers passed to initialize_stack_memory() must be arrays of this
 * type. It is chosen as uintptr_t for two reasons:
 *
 *   1. The allocator stores a pointer-sized bookkeeping value in the slot
 *      immediately before each coroutine frame. uintptr_t is guaranteed to
 *      hold a pointer value without truncation on any platform, including
 *      64-bit hosts used for testing.
 *
 *   2. The allocation arithmetic (mask, shift, word counts) is expressed in
 *      units of sizeof(uintptr_t), which equals the natural pointer size on
 *      every supported target.
 *
 * The backing array must be declared with alignas(std::max_align_t) to
 * satisfy the alignment contract of operator new. See initialize_stack_memory()
 * for details.
 */

export struct alignas(__STDCPP_DEFAULT_NEW_ALIGNMENT__) stack_word
{
  uintptr_t data = 0;
};

/**
 * @brief Bit mask for pointer alignment checking
 *
 * This constant represents a bitmask used to check if a pointer is properly
 * aligned to the word size (stack_word). It's calculated as
 * sizeof(stack_word) - 1.
 */
constexpr size_t mask = sizeof(stack_word) - 1uz;

/**
 * @brief Bit shift for word alignment calculations
 *
 * This constant represents the number of bits to shift when calculating
 * word-aligned memory requirements. It's calculated as
 * std::countr_zero(sizeof(stack_word)).
 */
constexpr size_t shift = std::countr_zero(sizeof(stack_word));
/**
 * @brief Enumeration of blocking states for async operations
 *
 * Describes what is blocking a coroutine from making forward progress.
 * Each state carries a distinct contract between the coroutine and the
 * scheduler regarding who is responsible for the transition back to nothing
 * and whether resumption is meaningful.
 *
 * The scheduler uses these states for two purposes:
 *
 *   1. Deciding which contexts to resume on each scan.
 *   2. Sleep eligibility — when every managed context is in time, signal,
 *      or sync states, the scheduler may sleep until the earliest time deadline
 *      OR set_listener() fires.
 */
export enum class blocked_by : std::uint8_t {
  /// Not blocked by anything, ready to run.
  ///
  /// The context has work it can make progress on and should be resumed by
  /// the scheduler at its earliest opportunity.
  nothing = 0,

  /// Blocked for a duration that must elapse before resuming.
  ///
  /// The scheduler is responsible for the unblock transition for this state. It
  /// must track the requested duration and call unblock() only after that
  /// duration has elapsed, after which it may resume the context.
  ///
  /// Resuming before the duration elapses is erroneous: it violates the
  /// timing contract of the coroutine (e.g., device reset windows, bus setup
  /// times). The resume() method enforces this by returning immediately if
  /// the context is still in this state.
  time = 1,

  /// Blocked by an asynchronous signal that will arrive externally.
  ///
  /// Something outside the scheduler's control will call unblock() when the
  /// operation completes. The source may be a hardware interrupt, a
  /// sender/receiver completion, an epoll callback, a kernel signal, or any
  /// other external event.
  ///
  /// The scheduler MUST NOT resume or manually unblock a context in this
  /// state. It should skip the context during its scan and rely on the
  /// set_listener() notification to know when the context becomes ready.
  signal = 2,

  /// Blocked by resource contention.
  ///
  /// The coroutine is waiting to acquire a resource currently held by another
  /// context (e.g., two coroutines competing for an I2C bus). Resuming a
  /// context in this state is permitted but typically pointless. The
  /// coroutine will re-check the resource, find it still held, and re-block.
  ///
  /// This state exists primarily for sleep eligibility: a scheduler that sees
  /// all of its contexts in time, signal, or sync knows that no productive
  /// work can happen and may sleep rather than busy-loop.
  ///
  /// When block_by_sync() is called, the context_listener's on_sync_block()
  /// callback is invoked with the address of the blocking context. A simple
  /// scheduler may ignore this and poll sync contexts on each scan. A more
  /// advanced scheduler may build dependency maps from these callbacks and
  /// perform targeted wake-ups when the blocker finishes.
  sync = 3,
};

/**
 * @brief Represents an async execution context for coroutines
 *
 * The context class manages coroutine execution, memory allocation, and
 * blocking states. It provides the infrastructure for running coroutines in
 * stack-based environments without heap allocation.
 *
 * Derived classes must:
 * 1. Provide stack memory via initialize_stack_memory()
 * 2. Implement do_schedule() to handle blocking state notifications
 *
 * The context is designed to be cache-line optimized (≤ 64 bytes) and supports
 * stack-based coroutine allocation. This makes it suitable for embedded systems
 * with limited memory resources.
 */
export class context;

/**
 * @brief Thrown when an async::context runs out of stack memory
 *
 * This exception is thrown when a coroutine attempts to allocate more memory
 * than is available in the context's stack buffer. It indicates that the
 * context has insufficient memory to accommodate the coroutine's execution
 * frame.
 *
 * @note The violator pointer may not be valid when caught, as the context might
 * have been destroyed during exception propagation.
 */
export struct bad_coroutine_alloc : std::bad_alloc
{
  /**
   * @brief Construct a bad_coroutine_alloc exception
   *
   * @param p_violator Pointer to the context that ran out of memory
   */
  bad_coroutine_alloc(context const* p_violator)
    : violator(p_violator)
  {
  }

  /**
   * @brief Get exception message
   *
   * @return C-string describing the error condition
   */
  [[nodiscard]] char const* what() const noexcept override
  {
    return "An async::context ran out of memory!";
  }

  /**
   * @brief A pointer to a context that ran out of memory
   *
   * NOTE: This pointer must NOT be assumed to be valid when caught. The
   * context could have been destroyed during propagation to the catch block.
   * The address MUST be compared against a valid and living context to
   * confirm they are the same. In the event the application can determine
   * that the violator has the same address of another known valid context,
   * then valid context should be accessed and NOT this pointer.
   *
   */
  context const* violator;
};

// =============================================================================
//
// Context
//
// =============================================================================

/**
 * @brief Sleep duration in microseconds
 *
 * Uses microsecond granularity and u32 representation for an optimal balance:
 *
 * - **Granularity**: Microseconds provide sufficient precision for real-time
 *   scheduling. Nanoseconds are too fine-grained—most systems cannot achieve
 *   that level of accuracy. Milliseconds are too coarse; many embedded
 *   schedulers regularly achieve 50-100µs precision with minimal context
 *   switching overhead.
 *
 * - **Range**: A u32 microseconds can represent durations up to ~4,294 seconds
 *   (1 hour 11 minutes), which is a practical upper bound for async operation
 *   delays. Longer delays should use alternative mechanisms or be broken into
 *   smaller segments.
 */
export using sleep_duration = std::chrono::duration<std::uint32_t, std::micro>;

/**
 * @brief Information about the block state when context::schedule is called
 *
 */
export using block_info =
  std::variant<std::monostate, sleep_duration, context*>;

class promise_base;

/**
 * @brief Interface for receiving notifications when an async context is
 * unblocked
 *
 * Implement this interface to receive notifications when a context transitions
 * from a blocked state to `blocked_by::nothing`. This is the primary mechanism
 * for schedulers to efficiently track which contexts become ready for execution
 * without polling.
 *
 * The implementation of `on_unblock()` may be called an interrupt service
 * routine thus it must be noexcept and interrupt service routine safe. Avoid
 * any operations that could block, allocate memory, or acquire non-ISR-safe
 * locks within `on_unblock()`.
 *
 * `on_sync_block()` communicates to the scheduler that one context is blocked
 * by another context, allowing the scheduler to decide how it wants to schedule
 * the context.
 */
export struct context_listener
{
public:
  virtual ~context_listener() = default;

private:
  friend class context;

  /**
   * @brief Called when a context transitions to the unblocked state
   *
   * This method is invoked by `context::unblock()` immediately after the
   * context's state is set to `blocked_by::nothing`. It signals to the
   * scheduler that the context is now ready to be resumed.
   *
   * @param p_context The context that has just been unblocked. The context's
   * state will be `blocked_by::nothing` at the time of this call. The
   * implementor may read any state from the context but MUST NOT resume or
   * destroy it within this call.
   *
   * @note This method MUST be noexcept and ISR-safe. It may be called from
   * any execution context including interrupt handlers.
   */
  virtual void on_unblock(context& p_context) noexcept
  {
    std::ignore = p_context;
  }

  /**
   * @brief Called when a context is blocked waiting for a synchronized resource
   *
   * This callback is invoked when a context becomes blocked because another
   * context currently owns the resource it needs.
   *
   * @param p_blocked - the context that is waiting for access to the resource
   * @param p_blocker - the address of the context that has control over the
   * resource that p_blocked needs.
   */
  virtual void on_sync_block(context& p_blocked,
                             context const& p_blocker) noexcept
  {
    std::ignore = p_blocked;
    std::ignore = p_blocker;
  }
};

/**
 * @brief The base context class for managing coroutine execution
 *
 * The context class is the core of the async_context library. It manages:
 *
 * - Stack-based coroutine allocation
 * - Coroutine execution state and blocking information
 * - Memory management for coroutine frames
 * - Scheduler integration through virtual methods
 *
 * Derived classes must implement the do_schedule() method to integrate with
 * custom schedulers and provide stack memory via initialize_stack_memory().
 *
 * @note Context objects should be kept alive as long as coroutines are running
 * on them. The context must be properly cleaned up to prevent memory leaks.
 */
export class context
{
public:
  // We store a single reference to a noop_coroutine() as multiple calls to this
  // function are not guaranteed to compare equal. In order to have a
  // noop_coroutine that plays nicely with our cancellation code, we need a
  // single handle for all to reference as a "done" state.
  inline static auto const noop_sentinel = std::noop_coroutine();
  static auto constexpr default_timeout = sleep_duration(0);

  /**
   * @brief Default constructor for context
   *
   * Creates context without a stack. `initialize_stack_memory()` must be called
   * before passing this context to a coroutine.
   */
  context() = default;

  /**
   * @brief Construct a new context object with stack memory.
   *
   * @param p_stack - the stack memory for the async operations
   */
  context(std::span<stack_word> p_stack)
  {
    initialize_stack_memory(p_stack);
  }

  /**
   * @brief Delete copy constructor
   *
   * Contexts cannot be copied as they manage unique stack memory.
   */
  context(context const&) = delete;

  /**
   * @brief Delete copy assignment operator
   *
   * Contexts cannot be copied as they manage unique stack memory.
   */
  context& operator=(context const&) = delete;

  /**
   * @brief Delete move constructor
   *
   * Contexts cannot be moved because the member variable m_stack_pointer's
   * address is stored within the context stack memory. Because moving the
   * object would cause the pointers within the stack to dangle, move is
   * deleted.
   */
  context(context&& p_other) noexcept = delete;

  /**
   * @brief Delete move assignment operator
   *
   * Contexts cannot be moved because the member variable m_stack_pointer's
   * address is stored within the context stack memory. Because moving the
   * object would cause the pointers within the stack to dangle, move is
   * deleted.
   */
  context& operator=(context&& p_other) noexcept = delete;

  /**
   * @brief Initialize stack memory for the context
   *
   * @param p_stack_memory - Stack memory provided by the derived context. It is
   * the responsibility of the derived context to manage this memory. If this
   * memory was dynamically allocated, then it is the responsibility of the
   * derived class to deallocate that memory.
   *
   * @note The span's base address must be aligned to at least
   * alignof(std::max_align_t) (8 bytes on ARM Cortex-M, 16 bytes on x86-64).
   * This matches the alignment guarantee of operator new and ensures that
   * coroutine frames containing double, int64_t, or std::chrono::duration are
   * correctly placed. The simplest way to satisfy this is to declare the
   * backing array with alignas(std::max_align_t):
   *
   * @code
   * alignas(std::max_align_t) std::array<async::stack_word, N> stack;
   * @endcode
   *
   * Passing a buffer that is only 4-byte aligned on a target where
   * alignof(std::max_align_t) == 8 is undefined behaviour and may produce
   * a hardfault if the coroutine frame requires 8-byte alignment.
   */
  constexpr void initialize_stack_memory(std::span<stack_word> p_stack_memory)
  {
    cancel();

    // NOTE: subtract 1 because we use the end of the stack for holding the
    //       length of the stack.
    auto const capacity = p_stack_memory.size() - 1uz;
    p_stack_memory.back().data = capacity;
    m_stack_pointer = &p_stack_memory.front();
    m_stack_end = &p_stack_memory.back();
  }

  /**
   * @brief Unblocks the context without invoking the unblock listener
   *
   * This method transitions the context to "nothing" (ready to run).
   */
  constexpr void unblock_without_notification() noexcept
  {
    get_original().m_state = blocked_by::nothing;
    get_original().m_sleep_time = sleep_duration::zero();
  }

  /**
   * @brief Unblocks the context and clears blocking state
   *
   * The state of the context after this is called:
   *
   *   1. Block state becomes (block_by::nothing)
   *   2. sleep time is set to 0us.
   *   3. sync blocker is set to nullptr.
   *
   * The unblock listener is called before clearing the context state in the
   * event that the unblock listener wants to inspect information from the
   * context.
   *
   * @note this API is safe to call within an interrupt service routine.
   */
  constexpr void unblock() noexcept
  {
    if (get_original().m_listener) {
      get_original().m_listener->on_unblock(*this);
    }

    // We clear this context state information after the unblock listener is
    // invoked to allow the unblock listener to inspect the context's current
    // state prior to being unblocked.
    unblock_without_notification();
  }

  /**
   * @brief Blocks the context for a specified time duration
   *
   * This method blocks the current coroutine for the specified duration,
   * transitioning it to the time-blocking state. The scheduler is responsible
   * for resuming this context after the duration has elapsed.
   *
   * @param p_duration The time duration to block for
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  template<typename Rep, typename Period>
  constexpr std::suspend_always block_by_time(
    std::chrono::duration<Rep, Period> p_duration) noexcept
  {
    get_original().m_state = blocked_by::time;
    get_original().m_sleep_time =
      std::chrono::duration_cast<sleep_duration>(p_duration);
    return {};
  }

  /**
   * @brief Signature for an io setup function
   *
   * This function accepts no arguments and takes no inputs. This function is
   * expected to setup I/O and kick off the I/O operation. The I/O operation
   * must be capable of asynchronously unblocking a context via an ISR or
   * other mechanism.
   *
   */
  using io_setup = void();

  /**
   * @brief Blocks the context for an I/O operation
   *
   * This method blocks the current coroutine until an I/O operation completes.
   * The context can be resumed by calling unblock() when the I/O is ready.
   *
   * @param p_duration Optional time estimate for when to poll or reschedule
   *                   the context again. The scheduler may ignore this hint.
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always block_by_signal(
    std::invocable<io_setup> auto p_setup,
    sleep_duration p_duration = default_timeout) noexcept
  {
    block_by_signal(p_duration);
    p_setup();
    return {};
  }

  /**
   * @brief Blocks the context for an I/O operation
   *
   * This method blocks the current coroutine until an I/O operation completes.
   * The context can be resumed by calling unblock() when the I/O is ready.
   *
   * @param p_duration Optional time estimate for when to poll or reschedule
   *                   the context again. The scheduler may ignore this hint.
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always block_by_signal(
    sleep_duration p_duration = default_timeout) noexcept
  {
    get_original().m_state = blocked_by::signal;
    get_original().m_sleep_time = p_duration;

    return {};
  }

  /**
   * @brief Blocks the context by resource contention (sync)
   *
   * This method blocks the current coroutine until a synchronization resource
   * becomes available. The context can be resumed at any time to check if the
   * resource can be claimed. A scheduler can collect the set of contexts
   * blocked by `p_blocker` and when `p_blocker` is no longer blocked by
   * anything, unblock and resume any of those context to have them acquire
   * access over the bus.
   *
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always block_by_sync(context const& p_blocker) noexcept
  {
    get_original().m_state = blocked_by::sync;
    if (m_listener != nullptr) {
      m_listener->on_sync_block(*this, p_blocker);
    }
    return {};
  }
  /**
   * @brief Get the current active coroutine handle
   *
   * This method returns the coroutine handle that is currently active on this
   * context.
   *
   * NOTE: It is UB to `destroy()` the returned handle. This coroutine is
   * managed/owned by this context and thus, the returned coroutine MUST NOT be
   * destroyed.
   *
   * @return std::coroutine_handle<> representing the active coroutine.
   */
  [[nodiscard]] constexpr std::coroutine_handle<> active_handle() const noexcept
  {
    return m_active_handle;
  }

  /**
   * @brief Get the current blocking state of this context
   *
   * This method returns the current blocking state that determines how the
   * scheduler should handle this context.
   *
   * @return blocked_by enum value indicating the current blocking state
   */
  [[nodiscard]] constexpr blocked_by state() const noexcept
  {
    if (m_awaited_context == nullptr) [[likely]] {
      return get_original().m_state;
    }
    return m_awaited_context->state();
  }

  /**
   * @brief Check if the context has completed its operation
   *
   * This method determines whether the context is in a "done" state, meaning
   * it has completed all operations and no longer needs to be scheduled. The
   * stack memory of the context should be completely unused when this function
   * returns `true`.
   *
   * @return true if the context is done, false otherwise
   */
  [[nodiscard]] constexpr bool done() const
  {
    return m_active_handle == noop_sentinel;
  }

  /**
   * @brief Cancel all operations on this context
   *
   * After cancellation, the state is `blocked_by::nothing`, the active
   * coroutine becomes the noop_sentinel and the stack memory is clear of any
   * living objects.
   *
   * This function must only be called when the coroutine is suspended.
   * A context may be cancelled when it is blocked.
   */
  void cancel();

  /**
   * @brief Resume the active coroutine on this context or do nothing if blocked
   *
   * This function  resumes the currently active coroutine if the context is
   * blocked by nothing. It only has an effect if the context is not blocked by
   * time, as time-blocking contexts must wait for their designated duration to
   * elapse.
   */
  void resume()
  {
    if (m_awaited_context == nullptr and
        get_original().m_state == blocked_by::nothing) [[likely]] {
      m_active_handle.resume();
    } else if (m_awaited_context != nullptr) {
      // This context is awaiting another context, check if its done
      if (m_awaited_context->done()) {
        m_awaited_context = nullptr;
        m_active_handle.resume();
      } else {
        // If the context is not done, resume the awaited context
        m_awaited_context->resume();
        // NOTE: The call above can be recursive if the awaited context is also
        // awaiting another context. This can occur all the way down until the
        // final leaf context is resumed. We expect such cases to be rare.
      }
    }
  }

  /**
   * @brief Perform sync_wait operation
   *
   * This method waits synchronously for all coroutines on this context to
   * complete. It uses the provided delay function to sleep for the required
   * duration when waiting for time-based operations.
   *
   * @tparam DelayFunc The type of the delay function (must be invocable with
   *                   sleep_duration parameter)
   * @param p_delay - a delay function, that accepts a sleep duration and
   *                  returns void.
   *
   * @note This method is primarily intended for testing and simple applications
   * where a synchronous wait is needed. It's not suitable for production
   * embedded systems that require precise timing or real-time scheduling.
   */
  void sync_wait(std::invocable<sleep_duration> auto&& p_delay)
  {
    while (not done()) {
      if (state() == blocked_by::time) {
        if (auto delay_time = sleep_time();
            delay_time > sleep_duration::zero()) {
          p_delay(delay_time);
        }
        unblock_without_notification();
      }
      resume();
    }
  }

  /**
   * @brief Get the total capacity of the stack memory
   *
   * This method returns the total size of the stack buffer in stack_word
   * words.
   *
   * @return The total capacity in stack_word words
   */
  [[nodiscard]] constexpr auto capacity() const noexcept
  {
    return m_stack_end->data;
  }

  /**
   * @brief Get the remaining stack memory available
   *
   * This method returns how much stack space is still available for new
   * coroutine allocation.
   *
   * @return The number of `stack_word` sized words used in the stack
   */
  [[nodiscard]] constexpr auto memory_remaining() const noexcept
  {
    return m_stack_end - m_stack_pointer;
  }

  /**
   * @brief Get the amount of stack memory used by active coroutines
   *
   * This method returns how much stack space has been consumed by currently
   * active coroutines.
   *
   * @return The number of `stack_word` sized words used in the stack
   */
  [[nodiscard]] constexpr auto memory_used() const noexcept
  {
    return capacity() - memory_remaining();
  }

  /**
   * @brief Amount of time to delay resuming this context
   *
   * When a context is blocked by for a set duration of time, it is the
   * responsibility of the scheduler to ensure that the context is not resumed
   * until that duration of time has elapsed. In the event that the context is
   * not blocked by time, then this returns 0.
   *
   * Calling this function multiple times returns the last sleep duration that
   * was set by the async operation contained within the context. It is the
   * responsibility of the scheduler to unblock this context, otherwise, calling
   * resume() will immediately without resuming async operation.
   *
   * @return constexpr sleep_duration - the amount of time to delay resuming
   * this context.
   */
  [[nodiscard]] constexpr sleep_duration sleep_time() const noexcept
  {
    if (m_awaited_context != nullptr) [[unlikely]] {
      return m_awaited_context->sleep_time();
    }
    return get_original().m_sleep_time;
  }

  /**
   * @brief Set the context listener
   *
   * There can only be a single unblock listener per context, thus any
   * previously set unblock listener will be removed.
   *
   * Because this API takes the address of the listener, it is important that
   * the context outlive the listener.
   *
   * `clear_listener()` must be called before the end of the lifetime of
   * the `p_listener` object. It is undefined behavior to allow a listener to be
   * destroyed before it is removed from this context.
   *
   * @param p_listener - the address of the unblock listener to be invoked when
   * this context is unblocked. A nullptr may be passed to this parameter. It
   * has the same effect as calling `clear_listener()`.
   */
  void set_listener(context_listener* p_listener)
  {
    get_original().m_listener = p_listener;
  }

  /**
   * @brief Clears the listener from this context
   *
   * After this is called, any call to `unblock()` will not invoke an unblock
   * listener.
   *
   * The application must clear the listener before the object passed to
   * set_listener() is destroyed.
   */
  void clear_listener() noexcept
  {
    get_original().m_listener = nullptr;
  }

  ~context()
  {
    cancel();
  }

private:
  template<typename T>
  friend class promise;
  friend class promise_base;
  friend class proxy_context;

  template<typename T>
  friend class future;

  /**
   * @brief Check if this is a proxy context
   *
   * This method determines whether the current context is acting as a proxy
   * for another context.
   *
   * @return true if this is a proxy context, false otherwise
   */
  [[nodiscard]] constexpr bool is_proxy() const noexcept
  {
    return m_original != nullptr;
  }

  /**
   * @brief Get a reference to the original context
   *
   * If this object is a proxy, then the original context will be pulled from
   * the proxy information and if its not, then a reference to `this` is
   * returned.
   *
   * @return context& - reference to the original context
   */
  [[nodiscard]] constexpr context const& get_original() const noexcept
  {
    if (is_proxy()) {
      return *m_original;
    } else {
      return *this;
    }
  }

  /**
   * @brief Get a reference to the original context
   *
   * If this object is a proxy, then the original context will be pulled from
   * the proxy information and if its not, then a reference to `this` is
   * returned.
   *
   * @return context& - reference to the original context
   */
  [[nodiscard]] constexpr context& get_original() noexcept
  {
    if (is_proxy()) {
      return *m_original;
    } else {
      return *this;
    }
  }

  /**
   * @brief Set the active coroutine handle for this context
   *
   * This method sets the coroutine that is currently running on this context.
   *
   * @param p_active_handle The coroutine handle to set as active
   */
  constexpr void active_handle(std::coroutine_handle<> p_active_handle)
  {
    m_active_handle = p_active_handle;
  }

  /**
   * @brief Allocate memory for a coroutine frame on the stack
   *
   * This method allocates space on the context's stack for a coroutine frame.
   * It ensures that the allocation fits within the available stack space.
   *
   * @param p_bytes The number of bytes to allocate
   * @return Pointer to the allocated memory location
   * @throws bad_coroutine_alloc if there's insufficient stack space
   */
  [[nodiscard]] constexpr void* allocate(std::size_t p_bytes)
  {
    // The extra 1 word is for the stack pointer's address
    size_t const words_to_allocate = 1uz + ((p_bytes + mask) >> shift);
    auto const new_stack_index = m_stack_pointer + words_to_allocate;

    if (new_stack_index > m_stack_end) [[unlikely]] {
      throw bad_coroutine_alloc(this);
    }

    // Put the address of the stack pointer member on the stack, before the
    // coroutine frame, such that the delete operation can find the address and
    // update it.
    m_stack_pointer->data =
      std::bit_cast<decltype(m_stack_pointer->data)>(&m_stack_pointer);

    // Address of the coroutine frame will be the current position of the stack
    // pointer + 1 to avoid overwriting the stack pointer address.
    auto* const coroutine_frame_stack_address = m_stack_pointer + 1uz;
    m_stack_pointer = new_stack_index;
    return coroutine_frame_stack_address;
  }

  // A concern for this library is how large the context objet is thus the word
  // sizes for each field is denoted below.
  //////////////////////////////////////////////////////--- // word 0
  blocked_by m_state = blocked_by::nothing;                 //   1B (u8) pad 4
  sleep_duration m_sleep_time = sleep_duration::zero();     //   4B (u32)
  std::coroutine_handle<> m_active_handle = noop_sentinel;  // word 1
  stack_word* m_stack_pointer = nullptr;                    // word 2
  stack_word* m_stack_end{};                                // word 3
  context_listener* m_listener = nullptr;                   // word 4
  context* m_original = nullptr;                            // word 5
  context* m_awaited_context = nullptr;                     // word 6
  context* m_awaiting_caller = nullptr;                     // word 7
};

/**
 * @brief A proxy context that provides isolated stack space for supervised
 * coroutines
 *
 * The proxy_context class allows creating a sub-context with its own stack
 * space that is isolated from the parent context. This is particularly useful
 * for implementing timeouts and supervision of coroutines.
 *
 * When a proxy context is created, it takes the remaining stack space from the
 * parent context and ensures that the parent's stack is properly clamped to
 * prevent overwrites.
 */
export class proxy_context : public context
{
public:
  /**
   * @brief Create a proxy context from an existing parent context
   *
   * This static method creates a new proxy context that uses a portion of the
   * parent context's stack memory. The proxy takes control over the remaining
   * stack space, effectively creating an isolated sub-context.
   *
   * @param p_parent The parent context to create a proxy from
   * @return A new proxy_context instance
   */
  static proxy_context from(context& p_parent)
  {
    return { p_parent };
  }

  /**
   * @brief Delete copy constructor
   *
   * Proxy contexts cannot be copied as they manage unique stack memory.
   */
  proxy_context(proxy_context const&) = delete;

  /**
   * @brief Delete copy assignment operator
   *
   * Proxy contexts cannot be copied as they manage unique stack memory.
   */
  proxy_context& operator=(proxy_context const&) = delete;

  /**
   * @brief Delete move constructor
   *
   * Proxy contexts cannot be moved as they manage unique stack memory.
   */
  proxy_context(proxy_context&&) = delete;

  /**
   * @brief Delete move assignment operator
   *
   * Proxy contexts cannot be moved as they manage unique stack memory.
   */
  proxy_context& operator=(proxy_context&&) = delete;

  constexpr void initialize_stack_memory(std::span<stack_word> p_stack_memory) =
    delete;

  /**
   * @brief Destructor for proxy_context
   *
   * The destructor cancels any remaining operations and properly restores
   * the parent context's stack memory to its original state.
   */
  ~proxy_context()
  {
    // Cancel any operations still on this context
    cancel();

    // Restore parent stack, by setting its range to be the start of its
    // stack and the end of our stack.
    m_parent->m_stack_end = m_stack_end;
  }

private:
  /**
   * @brief Constructor for proxy_context
   *
   * This private constructor is used internally to set up the proxy context
   * with isolated stack memory from a parent context.
   *
   * @param p_parent The parent context to create proxy from
   */
  proxy_context(context& p_parent)
    : m_parent(&p_parent)
  {
    m_active_handle = context::noop_sentinel;

    // We need to manually set:
    //    1. m_stack
    //    2. m_stack_pointer
    //    3. m_proxy

    // Our proxy will take control over the rest of the unused stack memory from
    // the above context.
    m_stack_pointer = p_parent.m_stack_pointer;
    m_stack_end = p_parent.m_stack_end;

    // Shrink the parent's stack to its current stack pointer, preventing it
    // from allocating again.
    p_parent.m_stack_end = p_parent.m_stack_pointer;

    // If this is a proxy, take its pointer to the origin
    if (p_parent.is_proxy()) {
      m_original = p_parent.m_original;
    } else {  // Otherwise, the current parent is the original
      m_original = &p_parent;
    }
    m_parent = &p_parent;
  }

  context* m_parent;
};

/**
 * @brief A context with embedded inplace stack memory
 *
 * The inplace_context class provides a concrete implementation of the context
 * with stack memory embedded directly in the object. Otherwise, it works
 * identically to the base context class.
 *
 * @tparam StackSizeInWords - the number of words to allocate for the context's
 * stack memory. Word size is 4 bytes for 32-bit systems and 8 bytes on 64-bit
 * systems.
 */
export template<size_t StackSizeInWords>
class inplace_context : public context
{
public:
  static_assert(StackSizeInWords > 0UL,
                "Stack memory must be greater than 0 words.");

  inplace_context()
    : context()
  {
    // NOTE: Passing m_stack to context() in the initializer list would
    // initialize the stack. But when inplace_context's constructor runs, it
    // clears the memory of m_stack, which would overwrite the capacity value
    // that initialize_stack_memory() writes into m_stack.back().
    //
    // And thus the line below is load bearing.
    initialize_stack_memory(m_stack);
  }

  inplace_context(inplace_context const&) = delete;
  inplace_context& operator=(inplace_context const&) = delete;
  inplace_context(inplace_context&&) = delete;
  inplace_context& operator=(inplace_context&&) = delete;

  ~inplace_context()
  {
    cancel();
  }

private:
  std::array<stack_word, StackSizeInWords> m_stack{};
};

// =============================================================================
//
// Promise Base
//
// =============================================================================

/**
 * @brief The base promise class for coroutine operations
 *
 * The promise_base class provides the foundation for coroutine promise types.
 * It handles memory allocation, coroutine state management, and integration
 * with the async_context system. It's designed to work with stack-based
 * allocation and provides the necessary infrastructure for coroutine frame
 * management.
 */
class promise_base
{
public:
  friend class context;

  // For regular functions
  template<typename... Args>
  static constexpr void* operator new(std::size_t p_size,
                                      context& p_context,
                                      Args&&...)
  {
    return p_context.allocate(p_size);
  }

  // For member functions - handles the implicit 'this' parameter
  template<typename Class, typename... Args>
  static constexpr void* operator new(std::size_t p_size,
                                      Class&,  // The 'this' object
                                      context& p_context,
                                      Args&&...)
  {
    return p_context.allocate(p_size);
  }

  /**
   * @brief Delete operator for coroutine promises
   *
   * This method handles cleanup of coroutine frames when they are destroyed.
   * It restores the stack pointer to its previous position, effectively
   * deallocating the memory used by the coroutine frame.
   *
   * @param p_promise Pointer to the promise being deleted
   */
  static constexpr void operator delete(void* p_promise) noexcept
  {
    // The stack that this promise is allocated on is in stack_word chunks.
    // Behind the promise's address is a stack_word that has the stack pointer's
    // address within its `data` member.
    auto* frame_header = static_cast<stack_word*>(p_promise) - 1uz;
    // The data field contains the address of the stack pointer, making it a
    // double pointer.
    auto** stack_pointer_ptr = std::bit_cast<stack_word**>(frame_header->data);
    // De-reference the stack_pointer_ptr to modify the stack_pointer_ptr and
    // assign it to the address of the frame_header
    *stack_pointer_ptr = frame_header;
  }

  /**
   * @brief Constructor for promise_base
   *
   * @param p_context The context that this promise will run on
   */
  promise_base(context& p_context)
    : m_context(&p_context)
  {
  }

  /**
   * @brief Constructor for promise_base with additional arguments
   *
   * @param p_context The context that this promise will run on
   * @param p_args Additional arguments for the constructor
   */
  template<typename... Args>
  promise_base(context& p_context, Args&&...)
    : m_context(&p_context)
  {
  }

  /**
   * @brief Constructor for member functions (handles 'this' parameter)
   *
   * @param p_this The 'this' object for member function promises
   * @param p_context The context that this promise will run on
   */
  template<typename Class>
  promise_base(Class&, context& p_context)
    : m_context(&p_context)
  {
  }

  /**
   * @brief Constructor for member functions with additional parameters
   *
   * @param p_this The 'this' object for member function promises
   * @param p_context The context that this promise will run on
   * @param p_args Additional arguments for the constructor
   */
  template<typename Class, typename... Args>
  promise_base(Class&, context& p_context, Args&&...)
    : m_context(&p_context)
  {
  }

  /**
   * @brief Get the initial suspend behavior for coroutines
   *
   * This method determines whether a coroutine should initially suspend
   * when it starts executing.
   *
   * @return std::suspend_always to suspend the coroutine initially
   */
  constexpr std::suspend_always initial_suspend() noexcept
  {
    return {};
  }

  /**
   * @brief Handle awaitable operations for sleep duration
   *
   * This method transforms sleep duration awaitables into appropriate blocking
   * operations for the context.
   *
   * @param p_sleep_duration The sleep duration to await
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  template<typename Rep, typename Ratio>
  constexpr auto await_transform(
    std::chrono::duration<Rep, Ratio> p_sleep_duration) noexcept
  {
    // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): False positive
    // clang-tidy assuming that m_context is uninitialized even though its
    // initialized at construction.
    return m_context->block_by_time(p_sleep_duration);
    // NOLINTEND(clang-analyzer-core.CallAndMessage)
  }

  /**
   * @brief Handle awaitable operations for I/O
   *
   * This method handles I/O operations that should block until completion.
   *
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always await_transform() noexcept
  {
    return m_context->block_by_signal();
  }

  /**
   * @brief Handle generic awaitables
   *
   * This method passes through any other awaitable operations without
   * modification.
   *
   * @param p_awaitable The awaitable to pass through
   * @return The original awaitable unchanged
   */
  template<typename U>
  constexpr U&& await_transform(U&& p_awaitable) noexcept
  {
    return static_cast<U&&>(p_awaitable);
  }

  /**
   * @brief Get the context associated with this promise
   *
   * @return Reference to the context that this promise runs on
   */
  constexpr auto& get_context()
  {
    return *m_context;
  }

  /**
   * @brief Get the continuation coroutine handle
   *
   * If the term "continuation" is confusing, another way of thinking about it
   * as the "return address" of the calling coroutine.
   *
   * @return The coroutine handle for the continuation of this operation
   */
  constexpr auto continuation()
  {
    return m_continuation;
  }

  /**
   * @brief Set the continuation coroutine handle
   *
   * @param p_continuation The coroutine handle to set as continuation
   */
  constexpr void continuation(std::coroutine_handle<> p_continuation)
  {
    m_continuation = p_continuation;
  }

  /**
   * @brief Pop the active coroutine from the context stack
   *
   * This method removes the current coroutine from the context's active handle
   * and returns its continuation.
   *
   * @return The coroutine handle for the continuation of this operation
   */
  constexpr std::coroutine_handle<> pop_active_coroutine()
  {
    m_context->active_handle(m_continuation);
    return m_continuation;
  }

  /**
   * @brief Cancel this coroutine operation
   *
   * This method cancels the current coroutine operation by setting its state
   * to cancelled and cleaning up resources.
   */
  void cancel()
  {
    // Set future state to cancelled
    m_cancel(this);
    // Pop self off context stack
    pop_active_coroutine();
    // Destroy promise objects & deallocate memory
    std::coroutine_handle<promise_base>::from_promise(*this).destroy();
  }

protected:
  /**
   * @brief Type alias for cancellation function pointer
   *
   * This type represents the function signature used for cancellation
   * callbacks.
   */
  using cancellation_fn = void(void*);

  // Consider m_continuation as the return address of the coroutine. The
  // coroutine handle for the coroutine that called and awaited the future that
  // generated this promise is stored here.
  std::coroutine_handle<> m_continuation = context::noop_sentinel;
  class context* m_context = nullptr;
  cancellation_fn* m_cancel = nullptr;
};

export template<typename T>
class future;

/**
 * @brief Type alias for conditional monostate or type T
 *
 * This alias provides a convenient way to represent either std::monostate (for
 * void) or the actual type T, depending on whether T is void. This is needed
 * for std::variant which cannot have a void type as a member.
 *
 * @tparam T The type to conditionally wrap with monostate
 */
template<typename T>
using monostate_or = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/**
 * @brief Represents a finished future of type void
 *
 * This struct is used as one of the states in a future's state variant to
 * represent that a void future has completed successfully.
 */
struct cancelled_state
{};

/**
 * @brief Represents a future that is currently busy.
 *
 * The purpose of this state is to report that a future is currently in a busy
 * state without exposing the coroutine handle.
 */
struct busy_state
{};

/**
 * @brief Defines the states that a future can be in
 *
 * This type alias defines the possible states a future can be in:
 *
 * - Running (suspended at await point)
 * - Value (completed with a value)
 * - Cancelled (explicitly cancelled)
 * - Exception (completed with an exception)
 *
 * @tparam T - the type for the future to eventually provide to the owner of
 * this future.
 */
export template<typename T>
using future_state =
  std::variant<std::coroutine_handle<>,  // 0 - running (the suspend case)
               monostate_or<T>,          // 1 - value (happy path!)
               cancelled_state,          // 2 - cancelled
               std::exception_ptr        // 3 - exception
               >;

/**
 * @brief Final awaiter for coroutine completion
 *
 * The final_awaiter is used to handle the final suspension point of a
 * coroutine. It ensures proper cleanup and continuation handling when a
 * coroutine completes.
 *
 * @tparam Promise The promise type for the coroutine
 */
template<class Promise>
struct final_awaiter
{
  /**
   * @brief Check if the awaiter is ready to resume
   *
   * @return false - always returns false to ensure await_suspend is always
   * called.
   */
  constexpr bool await_ready() noexcept
  {
    return false;
  }

  /**
   * @brief Suspend the coroutine when it completes
   *
   * This method handles the final suspension point and ensures proper
   * continuation of the calling coroutine.
   *
   * @param p_completing_coroutine The coroutine handle that is completing
   * @return The coroutine handle to resume next, which is a symmetric transfer
   * from this completing coroutine to its continuation (what called it
   * originally).
   */
  constexpr std::coroutine_handle<> await_suspend(
    std::coroutine_handle<Promise> p_completing_coroutine) noexcept
  {
    // The coroutine is now suspended at the final-suspend point.
    // Lookup its continuation in the promise and resume it symmetrically.
    //
    // Rather than return control back to the application, we continue the
    // caller function allowing it to yield when it reaches another suspend
    // point. The idea is that prior to this being called, we were executing
    // code and thus, when we resume the caller, we are still running code.
    // Lets continue to run as much code until we reach an actual suspend
    // point.
    auto next_to_run = p_completing_coroutine.promise().pop_active_coroutine();
    // Destroy promise at this point as there is no more use for it.
    p_completing_coroutine.destroy();
    return next_to_run;
  }

  /**
   * @brief Handle resume after completion
   *
   * This method is called when the awaiter resumes, but does nothing.
   */
  constexpr void await_resume() noexcept
  {
  }
};

/**
 * @brief Base class for promise return handling
 *
 * This class provides the infrastructure for handling return values from
 * coroutines in the promise system.
 *
 * @tparam T The type of value to return
 */
template<typename T>
struct promise_return_base
{
  /**
   * @brief Handle return value for non-void futures
   *
   * @param p_value The value to return from the coroutine
   */
  template<typename U>
  void return_value(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): clang-tidy incorrectly
    // assumes this pointer is uninitialized. The promise is constructed from
    // the future returned by `get_return_object()`, which properly initializes
    // this promise.
    m_future_state->template emplace<T>(std::forward<U>(p_value));
    // NOLINTEND(clang-analyzer-core.CallAndMessage)
  }

  /**
   * @brief Pointer to the future state that should be set at future<T>
   * construction.
   */
  future_state<T>* m_future_state = nullptr;
};

/**
 * @brief Specialization of promise_return_base for void futures
 *
 * This specialization handles return values for void futures.
 */
template<>
struct promise_return_base<void>
{
  /**
   * @brief Handle return void for void futures
   */
  void return_void() noexcept
  {
    // NOLINTBEGIN(clang-analyzer-core.CallAndMessage): clang-tidy incorrectly
    // assumes this pointer is uninitialized. The promise is constructed from
    // the future returned by `get_return_object()`, which properly initializes
    // this promise.
    *m_future_state = std::monostate{};
    // NOLINTEND(clang-analyzer-core.CallAndMessage)
  }

  /**
   * @brief Pointer to the future state that should be set at future<void>
   * construction.
   */
  future_state<void>* m_future_state = nullptr;
};

/**
 * @brief Promise class for coroutine operation handling
 *
 * The promise class is responsible for managing the lifecycle and state of
 * coroutine operations. It handles memory allocation, exception propagation,
 * and future state management.
 *
 * @tparam T The type of value this promise will eventually provide
 */
export template<typename T>
class promise
  : public promise_base
  , public promise_return_base<T>
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using promise_base::operator delete;
  using our_handle = std::coroutine_handle<promise<T>>;

  friend class future<T>;

  /**
   * @brief Get the final awaiter for coroutine completion
   *
   * @return The final_awaiter that handles completion of this coroutine
   */
  constexpr final_awaiter<promise<T>> final_suspend() noexcept
  {
    return {};
  }

  /**
   * @brief Handle unhandled exceptions in coroutines
   *
   * This method is called when a coroutine throws an exception that isn't
   * handled within the coroutine itself.
   */
  void unhandled_exception() noexcept
  {
    *promise_return_base<T>::m_future_state = std::current_exception();
  }

  /**
   * @brief Set future<T> object associated with this promise to cancelled state
   *
   * This static method is used to cancel a promise by setting its state to
   * cancelled_state. The exact promise type information is type erased and
   * saved into the promise_base such that the `context` class can safely set
   * its future objects to a cancelled state.
   *
   * @param p_self Pointer to the promise to cancel
   */
  static void cancel_promise(void* p_self)
  {
    auto* self = static_cast<promise<T>*>(p_self);
    *self->m_future_state = cancelled_state{};
  }

  /**
   * @brief Get the return object for this promise
   *
   * This method creates and returns a future that represents the result of
   * this coroutine operation.
   *
   * @return The future representing this coroutine's result
   */
  constexpr future<T> get_return_object() noexcept;
};

/**
 * @brief Represents a future value that can be awaited
 *
 * The future class represents the result of an asynchronous operation. It can
 * hold either a value, an exception, or be in progress (waiting for
 * completion). Futures are the primary way to manage asynchronous operations in
 * this library.
 *
 * @tparam T The type of value that this future will eventually hold
 */
export template<typename T>
class future
{
public:
  using promise_type = promise<T>;
  using handle_type = std::coroutine_handle<>;
  using full_handle_type = std::coroutine_handle<promise_type>;

  /**
   * @brief Thrown when a future is in the cancelled state and has been awaited
   * on
   *
   * This exception is thrown when a cancelled future is awaited. A future can
   * be in the cancelled state if its coroutine promise was destroyed.
   *
   * In general, normal usage of async context should never result in this
   * exception being thrown. In order for this to be seen, a future would need
   * to be created and then cancelled, then awaited on. This does not occur if a
   * future is awaited and then the context is cancelled.
   *
   */
  struct cancelled : public std::exception
  {
    cancelled(void const* p_future_address)
      : future_address(p_future_address)
    {
    }

    /**
     * @brief Get exception message
     *
     * @return C-string describing the cancellation error
     */
    [[nodiscard]] char const* what() const noexcept override
    {
      return "This future has been cancelled!";
    }

    void const* future_address = nullptr;
  };

  future(future const& p_other) = delete;
  future& operator=(future const& p_other) = delete;

  /**
   * @brief Default initialization for a void future
   *
   * This future will considered to be done on creation.
   *
   * @note For void futures, the state is initialized to std::monostate,
   * indicating completion with no return value.
   */
  future()
    requires(std::is_void_v<T>)
    : m_state(std::monostate{})
  {
  }

  /**
   * @brief Construct a future with a value
   *
   * This future will considered to be done and will contain just the value
   * passed into this.
   *
   * @tparam U - type that can construct a type T (which includes T itself)
   * @param p_value The value to initialize the future with
   *
   * @note This constructor creates a completed future containing the provided
   * value. The future will be in the "done" state with the value stored
   * internally.
   */
  template<typename U>
  constexpr future(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    m_state.template emplace<T>(std::forward<U>(p_value));
  };

  /**
   * @brief Move constructor for future
   *
   * Transfers ownership of the asynchronous operation from another future.
   *
   * @param p_other The future to move from
   *
   * @note After moving, the source future will be left in a valid but
   * unspecified state. The moved-to future will contain the same asynchronous
   * operation or result.
   */
  constexpr future(future&& p_other) noexcept
    : m_state(std::exchange(p_other.m_state, std::monostate{}))
  {
    if (std::holds_alternative<handle_type>(m_state)) {
      auto handle = std::get<handle_type>(m_state);
      full_handle_type::from_address(handle.address())
        .promise()
        .m_future_state = &m_state;
    }
  }

  /**
   * @brief Move assignment operator for future
   *
   * Transfers ownership of the asynchronous operation from another future.
   *
   * @param p_other The future to move from
   * @return Reference to this future
   *
   * @note After moving, the source future will be left in a valid but
   * unspecified state. The moved-to future will contain the same asynchronous
   * operation or result.
   */
  constexpr future& operator=(future&& p_other) noexcept
  {
    if (this != &p_other) {
      m_state = std::exchange(p_other.m_state, std::monostate{});
      if (std::holds_alternative<handle_type>(m_state)) {
        auto handle = std::get<handle_type>(m_state);
        full_handle_type::from_address(handle.address())
          .promise()
          .m_future_state = &m_state;
      }
    }
    return *this;
  }

  /**
   * @brief Destruct future
   *
   * If the future contins a coroutine handle on destruction, then cancel is
   * called on the associated context.
   */
  constexpr ~future()
  {
    if (std::holds_alternative<handle_type>(m_state)) {
      auto handle = std::get<handle_type>(m_state);
      full_handle_type::from_address(handle.address())
        .promise()
        .get_context()
        .cancel();
    }
  }

  constexpr bool is_cancelled()
  {
    return std::holds_alternative<cancelled_state>(m_state);
  }

  constexpr void resume() const
  {
    if (std::holds_alternative<handle_type>(m_state)) {
      auto handle = std::get<handle_type>(m_state);
      full_handle_type::from_address(handle.address())
        .promise()
        .get_context()
        .resume();
    }

    if (std::holds_alternative<std::exception_ptr>(m_state)) {
      std::rethrow_exception(std::get<std::exception_ptr>(m_state));
    }
  }

  /**
   * @brief Reports if this async object has finished its operation and now
   * contains a value.
   *
   * @return true - this operation is finished and either contains the value of
   * type T, an exception_ptr, or this future is in a cancelled state.
   * @return false - operation has yet to completed and does have a value.
   *
   * @note A future is considered "done" when it has either completed
   * successfully, encountered an exception, or been cancelled. This method can
   * be used to check if it's safe to extract the result or handle the
   * completion state.
   */
  [[nodiscard]] constexpr bool done() const
  {
    return not std::holds_alternative<handle_type>(m_state);
  }

  void cancel()
  {
    if (done()) {
      return;
    }

    auto handle = std::get<handle_type>(m_state);
    full_handle_type::from_address(handle.address())
      .promise()
      .get_context()
      .cancel();
  }

  /**
   * @brief Returns true if this future contains a value
   *
   * @return true - future contains a value
   * @return false - future does not contain a value
   */
  [[nodiscard]] constexpr bool has_value() const
  {
    return std::holds_alternative<monostate_or<T>>(m_state);
  }

  /**
   * @brief Extract value from async operation.
   *
   * @return Type - reference to the value from this async operation.
   * @throws std::bad_variant_access if `has_value()` return false
   *
   * @note This method should only be called when `has_value()` returns true.
   * Calling this method on a future that doesn't contain a value will throw
   * std::bad_variant_access.
   */
  [[nodiscard]] constexpr monostate_or<T>& value()
    requires(not std::is_void_v<T>)
  {
    return std::get<T>(m_state);
  }

  // Awaiter for when this task is awaited
  struct awaiter
  {
    future<T>& m_operation;

    constexpr explicit awaiter(future<T>& p_operation) noexcept
      : m_operation(p_operation)
    {
    }

    [[nodiscard]] constexpr bool await_ready() const noexcept
    {
      return not std::holds_alternative<handle_type>(m_operation.m_state);
    }

    /**
     * @brief Communicates to the awaiter to simply resume this coroutine
     * associated with this future.
     *
     * @tparam U - return type of the promise
     * @param p_calling_coroutine - this type is forgotten. The parent calling
     * coroutine's handle was captured when the future object was created via
     * get_return_object().
     * @return std::coroutine_handle<> - self to continue
     */
    template<typename U>
    std::coroutine_handle<> await_suspend(
      [[maybe_unused]] std::coroutine_handle<promise<U>>
        p_calling_coroutine) noexcept
    {
      auto handle = std::get<handle_type>(m_operation.m_state);
      auto& calling_ctx = p_calling_coroutine.promise().get_context();
      auto& awaited_ctx = full_handle_type::from_address(handle.address())
                            .promise()
                            .get_context();

      if (&calling_ctx != &awaited_ctx) [[unlikely]] {
        calling_ctx.m_awaited_context = &awaited_ctx;
        awaited_ctx.m_awaiting_caller = &calling_ctx;
      }

      return handle;
    }

    [[nodiscard]] constexpr monostate_or<T>&& await_resume() const
      requires(not std::is_void_v<T>)
    {
      if (std::holds_alternative<T>(m_operation.m_state)) [[likely]] {
        return std::move(std::get<T>(m_operation.m_state));
      } else if (std::holds_alternative<std::exception_ptr>(
                   m_operation.m_state)) [[unlikely]] {
        std::rethrow_exception(
          std::get<std::exception_ptr>(m_operation.m_state));
      } else if (std::holds_alternative<cancelled_state>(m_operation.m_state))
        [[unlikely]] {
        throw ::async::future<T>::cancelled{ &m_operation };
      }

      // In the event that this coroutine awaiting this awaitable has
      // requested the result of this awaitable before it has finished, then:
      //
      // - If contracts are enabled, then contract violation handler is called.
      // - Otherwise, std::terminate is called.
#if defined(__cpp_contracts)
      contract_assert(std::holds_alternative<handle_type>(m_operation.m_state));
#else
      std::terminate();
#endif
    }

    constexpr void await_resume() const
      requires(std::is_void_v<T>)
    {
      if (std::holds_alternative<std::monostate>(m_operation.m_state))
        [[likely]] {
        return;
      } else if (std::holds_alternative<std::exception_ptr>(
                   m_operation.m_state)) [[unlikely]] {
        std::rethrow_exception(
          std::get<std::exception_ptr>(m_operation.m_state));
      } else if (std::holds_alternative<cancelled_state>(m_operation.m_state))
        [[unlikely]] {
        throw ::async::future<T>::cancelled{ &m_operation };
      }

      // In the event that this coroutine awaiting this awaitable has
      // requested the result of this awaitable before it has finished, then:
      //
      // - If contracts are enabled, then contract violation handler is called.
      // - Otherwise, std::terminate is called.
#if defined(__cpp_contracts)
      contract_assert(std::holds_alternative<handle_type>(m_operation.m_state));
#else
      std::terminate();
#endif
    }
  };

  /**
   * @brief Provides the awaitable interface for use with `co_await`
   *
   * This method enables the future to be used in `co_await` expressions,
   * allowing other coroutines to wait for this future's completion.
   *
   * @return awaiter - An awaiter object that handles the suspension and
   * resumption of coroutines awaiting this future.
   *
   * @pre The coroutine's context and the future's context are the same.
   * @note The awaiter will suspend the calling coroutine until this future
   * completes, then resume with either the result value or an exception. The
   * future will never be cancelled.
   */
  [[nodiscard]] constexpr awaiter operator co_await() && noexcept
  {
    return awaiter{ *this };
  }

  /**
   * @brief co_awaiting an l-value future is banned
   *
   * Co-awaiting an l-value future provides a means to accidentally await on a
   * future with a different context than the awaiting coroutine. This is unsafe
   * and thus, banned.
   *
   * @return Nothing, deleted implementation
   */
  [[nodiscard]] constexpr awaiter operator co_await() & noexcept = delete;

private:
  friend promise_type;

  /**
   * @brief Note that this is the only handle type that can be assigned to
   * future state ensuring that from_address is always valid.
   *
   */
  explicit constexpr future(full_handle_type p_handle)
    : m_state(p_handle)
  {
    auto& promise = p_handle.promise();
    promise.m_future_state = &m_state;
    promise.m_cancel = &promise_type::cancel_promise;
  }

  future_state<T> m_state{};
};

/**
 * @brief Represents an async task (void future)
 *
 * The task type is an alias for future<void>, representing an asynchronous
 * operation that doesn't return a value. It's used for operations like
 * logging, I/O operations, or other async actions that don't need to return
 * a result.
 *
 * @note Task is equivalent to future<void> and serves as a convenient alias
 * for void-returning asynchronous operations.
 */
export using task = future<void>;

/**
 * @brief Get the return object for this promise
 *
 * This method creates and returns a future that represents the result of
 * this coroutine operation.
 *
 * @return The future representing this coroutine's result
 *
 * @note This method is called by the coroutine framework when a coroutine
 * completes. It creates a future that can be awaited by code that called
 * the coroutine.
 */
template<typename T>
constexpr future<T> promise<T>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<T>>;
  auto handle = future_handle::from_promise(*this);
  // Chain: whatever was active becomes our continuation.
  // If nothing was active (noop_sentinel), this is a normal top-level
  // coroutine. If something WAS active, we implicitly sit on top of it.
  m_continuation = m_context->active_handle();
  m_context->active_handle(handle);
  return future<T>{ handle };
}

/**
 * @brief Cancel all operations on this context
 *
 * This method cancels all pending operations on the context.
 *
 * A context awaiting this context will be disconnected from this context.
 * Meaning, if the context awaiting this context, when resumed will resume where
 * it left off. If a future with this context was awaited and was completed with
 * a value, then resuming the awaiting context will operate as normal. If the
 * future was cancelled before completing with a value or error, then resuming
 * the context and exiting the awaitable will result in the
 * `async::future::cancelled` exception type being throw.
 *
 * @note This method is called internally by the context destructor to ensure
 * proper cleanup of all pending asynchronous operations.
 */
void context::cancel()
{
  while (not done()) {
    std::coroutine_handle<promise_base>::from_address(m_active_handle.address())
      .promise()
      .cancel();
  }

  if (m_awaiting_caller != nullptr) {
    m_awaiting_caller->m_awaited_context = nullptr;
    m_awaiting_caller = nullptr;
  }
}
}  // namespace async::inline v0
