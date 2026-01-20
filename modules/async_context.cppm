// Copyright 2024 - 2025 Khalil Estell and the libhal contributors
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

#define DEBUGGING 0

#include <cstddef>
#include <cstdint>

#include <bit>
#include <chrono>
#include <coroutine>
#include <exception>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#if DEBUGGING
#include <print>
#endif

export module async_context;

namespace async::inline v0 {

/**
 * @brief Unsigned 8-bit integer type alias
 *
 * This is a type alias for std::uint8_t, used throughout the async_context
 * library for byte-sized operations.
 */
export using u8 = std::uint8_t;

/**
 * @brief Byte type alias
 *
 * This is a type alias for std::uint8_t, used to represent byte-sized values.
 * It's an alias for u8 and is provided for semantic clarity in contexts where
 * byte-level operations are performed.
 */
export using byte = std::uint8_t;

/**
 * @brief Size type alias
 *
 * This is a type alias for std::size_t, used to represent sizes and counts
 * throughout the async_context library.
 */
export using usize = std::size_t;

/**
 * @brief Unsigned pointer type alias
 *
 * This is a type alias for std::uintptr_t, used to represent pointer-sized
 * unsigned integer values.
 */
export using uptr = std::uintptr_t;

/**
 * @brief Bit mask for pointer alignment checking
 *
 * This constant represents a bitmask used to check if a pointer is properly
 * aligned to the word size (uptr). It's calculated as sizeof(uptr) - 1.
 */
constexpr size_t mask = sizeof(uptr) - 1uz;

/**
 * @brief Bit shift for word alignment calculations
 *
 * This constant represents the number of bits to shift when calculating
 * word-aligned memory requirements. It's calculated as
 * std::countr_zero(sizeof(uptr)).
 */
constexpr size_t shift = std::countr_zero(sizeof(uptr));

/**
 * @brief Enumeration of blocking states for async operations
 *
 * This enum describes the various states a coroutine can be in when blocked
 * from execution. Each state has different implications for how the scheduler
 * should handle resumption of the coroutine.
 *
 * The blocking states are ordered from least to most restrictive:
 * - nothing: Ready to run, no blocking
 * - time: Must wait for a specific duration before resuming
 * - io: Blocked by I/O operation that must complete
 * - sync: Blocked by resource contention (mutex, semaphore)
 * - external: Blocked by external coroutine system
 */
export enum class blocked_by : u8 {
  /// Not blocked by anything, ready to run, can be resumed.
  nothing = 0,

  /// Blocked by a time duration that must elapse before resuming.
  ///
  /// Another way of saying this is that the active coroutine is requesting to
  /// be rescheduled at or after the time duration provided. The sleep time
  /// provided is the minimum that a scheduler must wait before resuming the
  /// coroutine. If the coroutine is resumed earlier, then this is erroneous
  /// behavior. This behavior is clearly wrong, but is well defined. The
  /// coroutine will resume earlier than it had anticipated, which can cause
  /// other problems down the line. For example, if a coroutine resets a device
  /// and must wait 50ms before attempting to communicate with it again. If that
  /// time isn't upheld, then the code may thrown an exception when the device
  /// is not online by the time of resumption.
  ///
  /// This blocked by state is special in that it is not poll-able. Unlike the
  /// blocked by states below, when a coroutine requests to be rescheduled, the
  /// scheduler must ensure that the context/future it is bound to is resumed at
  /// the right time.
  time = 1,

  /// Blocked by an I/O operation (DMA, bus transaction, etc.).
  /// An interrupt or I/O completion will call unblock() when ready.
  ///
  /// This blocked by state is poll-able, meaning that the coroutine may be
  /// resumed before the context is unblocked.
  /// Coroutines MUST check that their I/O operations have completed before
  /// continuing on with their operations. If a coroutine is resumed and their
  /// I/O operation is still pending, those coroutines should block themselves
  /// by I/O again to signal to the scheduler that they are not ready yet.
  ///
  /// A time estimate may be provided to the scheduler to give extra information
  /// about when to poll or reschedule the context again. The time information
  /// may be ignored.
  io = 2,

  /// Blocked by a resource contention.
  ///
  /// Examples: mutex, semaphore, two coroutines competing for an I2C bus.
  ///
  /// If the coroutine is resumed, the coroutine should check that it can
  /// acquire the resource before assuming that it can. Just like I/O, if the
  /// coroutine determines that its still blocked by sync, then it must re-block
  /// itself by sync.
  sync = 3,

  /// Blocked by an external coroutine outside the async::context system.
  ///
  /// Examples: co_awaiting a std::task, std::generator, or third-party
  /// coroutine library.
  ///
  /// A coroutine invoking a 3rd party async library is considered to be a
  /// coroutine supervisor. A coroutine supervisor stays as the active coroutine
  /// on its context, and must manually resume the 3rd party async library until
  /// it finishes. This is important since the async context scheduler has no
  /// knowledge of the 3rd party async operation and how it works.
  ///
  /// If the external async library has the ability to call unblock() on the
  /// context, then it should, but is not mandated to do so. Like I/O, this is
  /// pollable by a scheduler and the coroutine code should block on external if
  /// the external coroutine is still active.
  external = 4,
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

/**
 * @brief Thrown when a coroutine awaits a cancelled future
 *
 * This exception is thrown when a coroutine attempts to await a future that
 * has been cancelled. It indicates that the operation was explicitly cancelled
 * before completion.
 */
export class operation_cancelled : public std::exception
{
  /**
   * @brief Get exception message
   *
   * @return C-string describing the cancellation error
   */
  [[nodiscard]] char const* what() const noexcept override
  {
    return "This future has been cancelled!";
  }
};

// =============================================================================
//
// Context
//
// =============================================================================

/**
 * @brief The data type for sleep time duration
 *
 */
export using sleep_duration = std::chrono::nanoseconds;

/**
 * @brief Information about the block state when context::schedule is called
 *
 */
export using block_info =
  std::variant<std::monostate, sleep_duration, context*>;

class promise_base;

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
   * Creates an uninitialized context. Derived classes must call
   * initialize_stack_memory() to properly set up the stack memory.
   */
  context() = default;

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
   * Contexts cannot be moved as they manage unique stack memory.
   */
  context(context&&) = delete;

  /**
   * @brief Delete move assignment operator
   *
   * Contexts cannot be moved as they manage unique stack memory.
   */
  context& operator=(context&&) = delete;

  /**
   * @brief Initialize stack memory for the context
   *
   * This method must be called by derived context implementations to provide
   * the stack memory that will be used for coroutine frame allocation.
   *
   * @param p_stack_memory - Stack memory provided by the derived context. It is
   * the responsibility of the derived context to manage this memory. If this
   * memory was dynamically allocated, then it is the responsibility of the
   * derived class to deallocate that memory.
   *
   * @note The stack memory must be properly aligned and sized to accommodate
   * coroutine frames. This is a required step for any derived context
   * implementation.
   */
  constexpr void initialize_stack_memory(std::span<uptr> p_stack_memory)
  {
    m_stack = p_stack_memory;
    m_stack_pointer = m_stack.data();
  }

  /**
   * @brief Unblocks a coroutine that was previously blocked
   *
   * This method transitions the context from any blocking state to "nothing"
   * (ready to run) state. It's typically called by I/O completion handlers or
   * when external conditions that were blocking a coroutine have been resolved.
   *
   * @note This method is noexcept and should not throw exceptions.
   * It's used internally by I/O completion handlers to signal that a blocked
   * coroutine can now proceed.
   */
  constexpr void unblock() noexcept
  {
    transition_to(blocked_by::nothing);
  }

  /**
   * @brief Unblocks a coroutine without notifying the scheduler
   *
   * This method transitions the context to "nothing" (ready to run) state but
   * does not call the scheduler's do_schedule method. This is useful in cases
   * where the scheduler state is being managed externally or during cleanup.
   *
   * @note This method is noexcept and should not throw exceptions.
   */
  constexpr void unblock_without_notification() noexcept
  {
    m_state = blocked_by::nothing;
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
  constexpr std::suspend_always block_by_time(
    sleep_duration p_duration) noexcept
  {
    transition_to(blocked_by::time, p_duration);
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
  constexpr std::suspend_always block_by_io(
    sleep_duration p_duration = default_timeout) noexcept
  {
    transition_to(blocked_by::io, p_duration);
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
   * @param p_blocker Pointer to the context that is currently blocking this one
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always block_by_sync(context* p_blocker) noexcept
  {
    transition_to(blocked_by::sync, p_blocker);
    return {};
  }

  /**
   * @brief Blocks the context by an external coroutine system
   *
   * This method blocks the current coroutine when it's awaiting an operation
   * from an external coroutine library (e.g., std::task, std::generator). The
   * coroutine is considered a supervising coroutine. The coroutine may be
   * resumed while blocked by external.
   *
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always block_by_external() noexcept
  {
    transition_to(blocked_by::external, std::monostate{});
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
  [[nodiscard]] constexpr auto state() const noexcept
  {
    return m_state;
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
  constexpr bool done()
  {
    return m_active_handle == noop_sentinel;
  }

  /**
   * @brief Cancel all operations on this context
   *
   * This method cancels all pending operations on this context.
   */
  void cancel();

  /**
   * @brief Resume the active coroutine on this context
   *
   * This method resumes the currently active coroutine. It only has an effect
   * if the context is not blocked by time, as time-blocking contexts must wait
   * for their designated duration to elapse.
   */
  void resume()
  {
    // We cannot resume the a coroutine blocked by time.
    // Only the scheduler can unblock a context state.
    if (m_state != blocked_by::time) {
      m_active_handle.resume();
    }
  }

  /**
   * @brief Get the amount of stack memory used by active coroutines
   *
   * This method returns how much stack space has been consumed by currently
   * active coroutines.
   *
   * @return The number of `uptr` sized words used in the stack
   */
  [[nodiscard]] constexpr auto memory_used() const noexcept
  {
    return m_stack_pointer - m_stack.data();
  }

  /**
   * @brief Get the total capacity of the stack memory
   *
   * This method returns the total size of the stack buffer in uptr words.
   *
   * @return The total capacity in uptr words
   */
  [[nodiscard]] constexpr auto capacity() const noexcept
  {
    return m_stack.size();
  }

  /**
   * @brief Get the remaining stack memory available
   *
   * This method returns how much stack space is still available for new
   * coroutine allocation.
   *
   * @return The number of `uptr` sized words used in the stack
   */
  [[nodiscard]] constexpr auto memory_remaining() const noexcept
  {
    return capacity() - memory_used();
  }

  /**
   * @brief Virtual destructor for proper cleanup of derived classes
   *
   * This virtual destructor ensures that derived context classes are properly
   * cleaned up when deleted through a base class pointer.
   */
  virtual ~context() = default;

private:
  friend class promise_base;
  template<typename T>
  friend class promise;

  /**
   * @brief Internal structure to track proxy context information
   */
  struct proxy_info
  {
    context* original = nullptr;
    context* parent = nullptr;
  };

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
    return m_proxy.parent == nullptr;
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
   * @brief Transition the context to a new blocking state
   *
   * This internal method transitions the context to a new blocking state and
   * notifies the scheduler via do_schedule().
   *
   * @param p_new_state The new blocking state to transition to
   * @param p_info Additional information about the blocking condition
   */
  constexpr void transition_to(blocked_by p_new_state,
                               block_info p_info = std::monostate{}) noexcept
  {
    m_state = p_new_state;
    schedule(p_new_state, p_info);
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

    if (new_stack_index > &m_stack.back()) [[unlikely]] {
      throw bad_coroutine_alloc(this);
    }

    // Put the address of the stack pointer member on the stack, before the
    // coroutine frame, such that the delete operation can find the address and
    // update it.
    *m_stack_pointer = std::bit_cast<uptr>(&m_stack_pointer);
#if DEBUGGING
    std::println("💾 Allocating {} words, current stack {}, new stack {}, "
                 "stack pointer member address: 0x{:x}",
                 words_to_allocate,
                 static_cast<void*>(m_stack_pointer),
                 static_cast<void*>(new_stack_index),
                 *m_stack_pointer);
#endif
    // Address of the coroutine frame will be the current position of the stack
    // pointer + 1 to avoid overwriting the stack pointer address.
    auto* const coroutine_frame_stack_address = m_stack_pointer + 1uz;
    m_stack_pointer = new_stack_index;
    return coroutine_frame_stack_address;
  }

  /**
   * @brief Wrapper around call to do_schedule
   *
   * This wrapper exists to allow future extensibility
   *
   * @param p_block_state - state that this context has been set to
   * @param p_block_info - information about the blocking conditions
   */
  void schedule(blocked_by p_block_state, block_info p_block_info) noexcept
  {
    return do_schedule(p_block_state, p_block_info);
  }

  /**
   * @brief Implementations of context use this to notify their scheduler of
   * changes to this async context.
   *
   * It is up to the scheduler to ensure that concurrent calls to this API are
   * serialized appropriately. For a single threaded event loop, syncronization
   * and serialization is not necessary. For a thread pool implementation,
   * syncronization and serialization must be considered.
   *
   * @param p_block_state - the type of blocking event the context has
   * occurred.
   * @param p_block_info - Information about what exactly is blocking this
   * context. If p_block_info is a sleep_duration, and the p_block_state is
   * blocked_by::time, then this context is requesting to be scheduled at that
   * or a later time. If the p_block_info is a sleep_duration, and the block
   * state isn't blocked_by::time, then this sleep duration is a hint to the
   * scheduler to when it would be appropriate to reschedule this context. The
   * scheduler does not have to be abided by this. If p_block_info is a pointer
   * to a context, then the pointed to context is currently blocking p_context.
   * This can be used to determine when to schedule p_context again, but does
   * not have to be abided by for proper function.
   */
  virtual void do_schedule(blocked_by p_block_state,
                           block_info p_block_info) noexcept = 0;
  friend class proxy_context;

  /* vtable ptr */                                          // word 1
  std::coroutine_handle<> m_active_handle = noop_sentinel;  // word 2
  std::span<uptr> m_stack{};                                // word 3-4
  uptr* m_stack_pointer = nullptr;                          // word 5
  blocked_by m_state = blocked_by::nothing;                 // word 6
  proxy_info m_proxy{};                                     // word 7-8
};

// Context should stay close to a standard cache-line of 64 bytes (8 words) for
// a 64-bit system. This compile time check ensures that the context does not
// exceed the this boundary for the platform.
static_assert(sizeof(context) <= std::hardware_constructive_interference_size,
              "Context cannot be contained within a cache-line (as specified "
              "by std::hardware_constructive_interference_size)");

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
   * @brief Destructor for proxy_context
   *
   * The destructor cancels any remaining operations and properly restores
   * the parent context's stack memory to its original state.
   */
  ~proxy_context() override
  {
    // Cancel any operations still on this context
    cancel();

    // Restore parent stack, by setting its range to be the start of its
    // stack and the end of our stack.
    m_proxy.parent->m_stack = { m_proxy.parent->m_stack.begin(),
                                m_stack.end() };
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
  {
    m_active_handle = context::noop_sentinel;
    m_proxy = {};

    // We need to manually set:
    //    1. m_stack
    //    2. m_stack_pointer
    //    3. m_proxy

    // Our proxy will take control over the rest of the unused stack memory from
    // the above context.
    auto remaining_words = p_parent.m_stack_pointer - p_parent.m_stack.data();
    m_stack = p_parent.m_stack.last(remaining_words);
    m_stack_pointer = m_stack.data();

    // Shrink the parent's stack to its current stack pointer, preventing it
    // from allocating again.
    p_parent.m_stack = { p_parent.m_stack.data(), p_parent.m_stack_pointer };

    // If this is a proxy, take its pointer to the origin
    if (p_parent.is_proxy()) {
      m_proxy = proxy_info{
        .original = m_proxy.original,
        .parent = &p_parent,
      };
    } else {  // Otherwise, the current parent is the origin.
      m_proxy = proxy_info{
        .original = &p_parent,
        .parent = &p_parent,
      };
    }
  }

  /**
   * @brief Forwards the schedule call to the original context
   *
   * This method forwards scheduling notifications to the original context,
   * ensuring that the parent context's scheduler is properly notified of
   * state changes.
   *
   * @param p_block_state - state that this context has been set to
   * @param p_block_info - information about the blocking conditions
   */
  void do_schedule(blocked_by p_block_state,
                   block_info p_block_info) noexcept override
  {
    m_proxy.original->schedule(p_block_state, p_block_info);
  }
};

/**
 * @brief A basic context implementation that supports synchronous waiting
 *
 * The basic_context class provides a concrete implementation of the context
 * interface that supports synchronous waiting operations. It extends the base
 * context with functionality to wait for coroutines to complete using a simple
 * synchronous loop.
 *
 * NOTE: This class does not provide stack memory
 *
 * This context is particularly useful for testing and simple applications where
 * a scheduler isn't needed, as it provides a way to wait for all coroutines to
 * complete without requiring external scheduling.
 *
 * @note basic_context is designed for simple use cases and testing, not
 * production embedded systems where strict memory management is required.
 */
export class basic_context : public context
{
public:
  // TODO(63): Add stack memory template argument
  /**
   * @brief Default constructor for basic_context
   *
   * Creates a new basic context with default initialization.
   */
  basic_context() = default;

  /**
   * @brief Virtual destructor for proper cleanup
   *
   * Ensures that the basic context is properly cleaned up when deleted.
   */
  ~basic_context() override = default;

  /**
   * @brief Get the pending delay time for time-blocking operations
   *
   * This method returns the sleep duration that is currently pending for
   * time-blocking operations. It's used by the sync_wait method to determine
   * how long to sleep.
   *
   * @return The pending sleep duration
   *
   * @note This is an internal method used by the basic_context implementation
   * to manage time-based blocking operations during synchronous waiting.
   */
  [[nodiscard]] constexpr sleep_duration pending_delay() const noexcept
  {
    return m_pending_delay;
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
    while (active_handle() != context::noop_sentinel) {
      active_handle().resume();

      if (state() == blocked_by::time) {
        if (m_pending_delay == sleep_duration(0)) {
          unblock_without_notification();
          continue;
        }
        p_delay(m_pending_delay);
        m_pending_delay = sleep_duration(0);
        unblock_without_notification();
      }
    }
  }

private:
  /**
   * @brief Forwards the schedule call to the original context
   *
   * This method handles scheduling notifications for time-blocking operations.
   * It stores the pending delay duration so that sync_wait can properly wait
   * for it.
   *
   * @param p_block_state - state that this context has been set to
   * @param p_block_info - information about the blocking conditions
   */
  void do_schedule(blocked_by p_block_state,
                   block_info p_block_info) noexcept final
  {
    if (p_block_state == blocked_by::time) {
      if (auto* ex = std::get_if<sleep_duration>(&p_block_info)) {
        m_pending_delay = *ex;
      } else {
        m_pending_delay = sleep_duration{ 0 };
      }
    }
    // Ignore the rest and poll them...
  }

  /**
   * @brief The pending delay for time-blocking operations
   *
   * This member stores the sleep duration that is currently pending for
   * time-blocking operations, allowing sync_wait to properly handle delays.
   */
  sleep_duration m_pending_delay{ 0 };
};

/**
 * @brief A RAII-style guard for exclusive access to a context
 *
 * The exclusive_access class provides a mechanism for managing exclusive
 * access to a context, particularly in scenarios involving synchronization
 * primitives like mutexes or semaphores. It ensures proper cleanup and
 * unblocking when the guard goes out of scope.
 *
 * This is particularly useful for implementing resource management in
 * coroutine-based systems where proper cleanup and blocking state
 * transitions are required.
 */
export class exclusive_access
{
public:
  /**
   * @brief Default constructor for exclusive_access
   *
   * Creates an uninitialized exclusive_access guard.
   */
  constexpr exclusive_access() = default;

  /**
   * @brief Constructor that captures a context for exclusive access
   *
   * @param p_capture The context to capture for exclusive access
   */
  constexpr exclusive_access(context& p_capture) noexcept
    : m_context_address(&p_capture)
  {
  }

  /**
   * @brief Assignment operator to capture a new context
   *
   * @param p_capture The context to capture for exclusive access
   * @return Reference to this exclusive_access instance
   */
  constexpr exclusive_access& operator=(context& p_capture) noexcept
  {
    m_context_address = &p_capture;
    return *this;
  }

  /**
   * @brief Assignment operator to clear the context capture
   *
   * @param p_capture nullptr to clear the capture
   * @return Reference to this exclusive_access instance
   */
  constexpr exclusive_access& operator=(nullptr_t) noexcept
  {
    m_context_address = nullptr;
    return *this;
  }

  /**
   * @brief Copy constructor for exclusive_access
   *
   * @param p_capture The exclusive_access instance to copy from
   */
  constexpr exclusive_access(exclusive_access const& p_capture) noexcept =
    default;

  /**
   * @brief Copy assignment operator for exclusive_access
   *
   * @param p_capture The exclusive_access instance to copy from
   * @return Reference to this exclusive_access instance
   */
  constexpr exclusive_access& operator=(
    exclusive_access const& p_capture) noexcept = default;

  /**
   * @brief Move constructor for exclusive_access
   *
   * @param p_capture The exclusive_access instance to move from
   */
  constexpr exclusive_access(exclusive_access&& p_capture) noexcept = default;

  /**
   * @brief Move assignment operator for exclusive_access
   *
   * @param p_capture The exclusive_access instance to move from
   * @return Reference to this exclusive_access instance
   */
  constexpr exclusive_access& operator=(exclusive_access& p_capture) noexcept =
    default;

  /**
   * @brief Equality operator to check if this guard holds a specific context
   *
   * @param p_context The context to compare against
   * @return true if this guard holds the specified context, false otherwise
   */
  constexpr bool operator==(context& p_context) noexcept
  {
    return m_context_address == &p_context;
  }

  /**
   * @brief Check if this guard is currently holding a context
   *
   * @return true if the guard has an active context, false otherwise
   */
  [[nodiscard]] constexpr bool in_use() const noexcept
  {
    return m_context_address != nullptr;
  }

  /**
   * @brief Check if this guard has an active context (bool conversion)
   *
   * This operator provides a way to check if the guard is currently active.
   *
   * @return true if the guard has an active context, false otherwise
   */
  [[nodiscard]] auto address() const noexcept
  {
    return m_context_address != nullptr;
  }

  /**
   * @brief Convert to bool (check if in use)
   *
   * This operator provides a way to check if the guard is currently active.
   *
   * @return true if the guard has an active context, false otherwise
   */
  [[nodiscard]] constexpr operator bool() const noexcept
  {
    return in_use();
  }

  /**
   * @brief Set this guard as a blocking state for synchronization
   *
   * This method sets the specified context to be blocked by synchronization,
   * effectively creating a dependency between contexts.
   *
   * @param p_capture The context to set as blocking by sync
   * @return std::suspend_always to suspend the coroutine until resumed
   */
  constexpr std::suspend_always set_as_block_by_sync(context& p_capture)
  {
    if (in_use()) {
      p_capture.block_by_sync(m_context_address);
    }
    return {};
  }

  /**
   * @brief Unblocks the associated context and clears this guard
   *
   * This method unblocks the context that was captured by this guard and
   * clears the guard's reference to it.
   */
  constexpr void unblock_and_clear() noexcept
  {
    if (in_use()) {
      m_context_address->unblock();
      m_context_address = nullptr;
    }
  }

private:
  /**
   * @brief The address of the context being held, or nullptr if not in use
   */
  context* m_context_address = nullptr;
};

/**
 * @brief I/O operation descriptor for async operations
 *
 * The io struct provides a way to describe I/O operations that can be awaited.
 * It contains information about the expected duration for I/O completion,
 * which can be used by schedulers to determine when to poll or reschedule
 * coroutines waiting for I/O operations.
 */
export struct io
{
  /**
   * @brief Construct an io descriptor with a specified duration
   *
   * @param p_duration The expected duration for I/O completion (default: 0)
   */
  io(sleep_duration p_duration = sleep_duration{ 0u })
    : m_duration(p_duration)
  {
  }

  /**
   * @brief The expected duration for I/O completion
   *
   * This field represents the estimated time for an I/O operation to complete.
   * It can be used by schedulers to determine appropriate polling intervals
   * or scheduling decisions.
   */
  sleep_duration m_duration;
};

/**
 * @brief Synchronization operation descriptor for async operations
 *
 * The sync struct provides a way to describe synchronization operations that
 * can be awaited. It contains a reference to an exclusive_access guard,
 * which is used to manage resource contention in coroutine-based systems.
 */
export struct sync
{
  /**
   * @brief Construct a sync descriptor with an exclusive access guard
   *
   * @param p_context The exclusive access guard that describes the sync
   * operation
   */
  sync(exclusive_access p_context)
    : m_context(p_context)
  {
  }

  /**
   * @brief The exclusive access guard for this synchronization operation
   *
   * This field holds the exclusive access information that describes the
   * synchronization resource being waited for.
   */
  exclusive_access m_context;
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
    // Acquire the pointer to the context stack memory from behind the coroutine
    // frame's memory.
    auto** stack_pointer_address = *(static_cast<uptr***>(p_promise) - 1);
#if DEBUGGING
    std::println(
      "Deleting {}, context's stack address ptr is at {}. Moving stack "
      "pointer to = {}, stack pointer was previously = {}",
      p_promise,
      static_cast<void*>(stack_pointer_address),
      static_cast<void*>(static_cast<uptr*>(p_promise) - 1),
      static_cast<void*>(*stack_pointer_address));
#endif
    // Update the stack pointer's address to be equal where it was before the
    // promise was allocated. Or said another way, the
    *stack_pointer_address = (static_cast<uptr*>(p_promise) - 1);
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
    return m_context->block_by_time(p_sleep_duration);
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
    m_context->block_by_io();
    return {};
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
   * @return The coroutine handle to resume next, which is a symmetryic transfer
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
    // set future to its awaited T value
    m_future_state->template emplace<T>(std::forward<U>(p_value));
  }

  /**
   * @brief Pointer to the future state that should be set at future<T>
   * construction.
   */
  future_state<T>* m_future_state;
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
    *m_future_state = std::monostate{};
  }

  /**
   * @brief Pointer to the future state that should be set at future<void>
   * construction.
   */
  future_state<void>* m_future_state;
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
        .set_object_address(&m_state);
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
          .set_object_address(&m_state);
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

    constexpr explicit awaiter(future<T>& p_operation
                               [[clang::lifetimebound]]) noexcept
      : m_operation(p_operation)
    {
    }

    [[nodiscard]] constexpr bool await_ready() const noexcept
    {
      return m_operation.m_state.index() >= 1;
    }

    template<typename U>
    std::coroutine_handle<> await_suspend(
      std::coroutine_handle<promise<U>> p_calling_coroutine) noexcept
    {
      // This will not throw because the discriminate check was performed in
      // `await_ready()` via the done() function. `done()` checks if the state
      // is `handle_type` and if it is, it returns false, causing the code to
      // call await_suspend().
      auto handle = std::get<handle_type>(m_operation.m_state);
      std::coroutine_handle<promise<U>>::from_address(handle.address())
        .promise()
        .continuation(p_calling_coroutine);
      return handle;
    }

    constexpr monostate_or<T>& await_resume() const
      requires(not std::is_void_v<T>)
    {
      if (std::holds_alternative<T>(m_operation.m_state)) [[likely]] {
        return std::get<T>(m_operation.m_state);
      } else if (std::holds_alternative<std::exception_ptr>(
                   m_operation.m_state)) [[unlikely]] {
        std::rethrow_exception(
          std::get<std::exception_ptr>(m_operation.m_state));
      }

      throw operation_cancelled{};
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
      }

      throw operation_cancelled{};
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
   * @note The awaiter will suspend the calling coroutine until this future
   * completes, then resume with either the result value or an exception. The
   * future will never be cancelled.
   */
  [[nodiscard]] constexpr awaiter operator co_await() noexcept
  {
    return awaiter{ *this };
  }

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
  m_context->active_handle(handle);
  return future<T>{ handle };
}

/**
 * @brief Cancel all operations on this context
 *
 * This method cancels all pending operations on the context.
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
}
}  // namespace async::inline v0
