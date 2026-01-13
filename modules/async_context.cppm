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

export using u8 = std::uint8_t;
export using byte = std::uint8_t;
export using usize = std::size_t;
export using uptr = std::uintptr_t;

constexpr size_t mask = sizeof(uptr) - 1uz;
constexpr size_t shift = std::countr_zero(sizeof(uptr));

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

export class context;

/**
 * @brief Thrown when an async::context runs out of stack memory
 *
 * This occurs if a coroutine co_awaits a function and the coroutine promise
 * cannot fit withint he context.
 *
 */
export struct bad_coroutine_alloc : std::bad_alloc
{
  bad_coroutine_alloc(context const* p_violator)
    : violator(p_violator)
  {
  }

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
 */
export class operation_cancelled : public std::exception
{
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

export class context
{
public:
  static auto constexpr default_timeout = sleep_duration(0);

  context() = default;
  context(context const&) = delete;
  context& operator=(context const&) = delete;
  context(context&&) = delete;
  context& operator=(context&&) = delete;

  /**
   * @brief Implementations of context must call this API in their constructor
   * in order to initialize the stack memory of this context.
   *
   * @param p_stack_memory - stack memory provided by the derived context. It is
   * the responsibility of the derived context to manager this memory. If this
   * memory was dynamically allocated, then it is the responsibility of the
   * derived class to deallocate that memory.
   */
  constexpr void initialize_stack_memory(std::span<uptr> p_stack_memory)
  {
    m_stack = p_stack_memory;
    m_stack_pointer = m_stack.data();
  }

  constexpr void unblock() noexcept
  {
    transition_to(blocked_by::nothing);
  }

  constexpr void unblock_without_notification() noexcept
  {
    m_state = blocked_by::nothing;
  }

  constexpr std::suspend_always block_by_time(
    sleep_duration p_duration) noexcept
  {
    transition_to(blocked_by::time, p_duration);
    return {};
  }

  constexpr std::suspend_always block_by_io(
    sleep_duration p_duration = default_timeout) noexcept
  {
    transition_to(blocked_by::io, p_duration);
    return {};
  }

  constexpr std::suspend_always block_by_sync(context* p_blocker) noexcept
  {
    transition_to(blocked_by::sync, p_blocker);
    return {};
  }

  constexpr std::suspend_always block_by_external() noexcept
  {
    transition_to(blocked_by::external, std::monostate{});
    return {};
  }

  [[nodiscard]] constexpr std::coroutine_handle<> active_handle() const noexcept
  {
    return m_active_handle;
  }

  [[nodiscard]] constexpr auto state() const noexcept
  {
    return m_state;
  }

  constexpr void active_handle(std::coroutine_handle<> p_active_handle)
  {
    m_active_handle = p_active_handle;
  }

  constexpr bool done()
  {
    return m_active_handle == std::noop_coroutine();
  }

  void resume()
  {
    // We cannot resume the a coroutine blocked by time.
    // Only the scheduler can unblock a context state.
    if (m_state != blocked_by::time) {
      m_active_handle.resume();
    }
  }

  /**
   * @brief Unsafe cancel will cancel a context's async operation
   *
   * This operation is labelled as "unsafe" because it this API does not update
   * the top level  future<T> object that was initially bound to this context,
   * to the "cancelled" state. Because of this, using/accessing that future<T>
   * in anyway is considered UB.
   *
   */
  void unsafe_cancel();

  [[nodiscard]] constexpr auto memory_used() const noexcept
  {
    return m_stack_pointer - m_stack.data();
  }

  [[nodiscard]] constexpr auto capacity() const noexcept
  {
    return m_stack.size();
  }

  [[nodiscard]] constexpr auto memory_remaining() const noexcept
  {
    return capacity() - memory_used();
  }

  // TODO(#40): Perform cancellation on context destruction
  virtual ~context() = default;

private:
  friend class promise_base;
  template<typename T>
  friend class promise;

  struct proxy_info
  {
    context* original = nullptr;
    context* parent = nullptr;
  };

  [[nodiscard]] constexpr bool is_proxy() const noexcept
  {
    return m_proxy.parent == nullptr;
  }

  constexpr void transition_to(blocked_by p_new_state,
                               block_info p_info = std::monostate{}) noexcept
  {
    m_state = p_new_state;
    schedule(p_new_state, p_info);
  }

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

  /* vtable ptr */                                                  // word 1
  std::coroutine_handle<> m_active_handle = std::noop_coroutine();  // word 2
  std::span<uptr> m_stack{};                                        // word 3-4
  uptr* m_stack_pointer = nullptr;                                  // word 5
  blocked_by m_state = blocked_by::nothing;                         // word 6
  proxy_info m_proxy{};                                             // word 7-8
};

// Context should stay close to a standard cache-line of 64 bytes (8 words) for
// a 64-bit system. This compile time check ensures that the context does not
// exceed the this boundary for the platform.
static_assert(sizeof(context) <= std::hardware_constructive_interference_size,
              "Context cannot be contained within a cache-line (as specified "
              "by std::hardware_constructive_interference_size)");

export class proxy_context : public context
{
public:
  proxy_context(proxy_context const&) = delete;
  proxy_context& operator=(proxy_context const&) = delete;
  proxy_context(proxy_context&&) = delete;
  proxy_context& operator=(proxy_context&&) = delete;

  static proxy_context from(context& p_parent)
  {
    return { p_parent };
  }

  ~proxy_context() override
  {
    // Unshrink parent stack, by setting its range to be the start of its
    // stack and the end to be the end of this stack.
    m_proxy.parent->m_stack = { m_proxy.parent->m_stack.begin(),
                                m_stack.end() };
  }

private:
  proxy_context(context& p_parent)
  {
    m_active_handle = std::noop_coroutine();
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
   * @param p_block_state - state that this context has been set to
   * @param p_block_info - information about the blocking conditions
   */
  void do_schedule(blocked_by p_block_state,
                   block_info p_block_info) noexcept override
  {
    m_proxy.original->schedule(p_block_state, p_block_info);
  }
};

export class basic_context : public context
{
public:
  basic_context() = default;
  ~basic_context() override = default;

  [[nodiscard]] constexpr sleep_duration pending_delay() const noexcept
  {
    return m_pending_delay;
  }

  /**
   * @brief Perform sync_wait operation
   *
   * @tparam DelayFunc
   * @param p_delay - a delay function, that accepts a sleep duration and
   * returns void.
   */
  void sync_wait(std::invocable<sleep_duration> auto&& p_delay)
  {
    while (active_handle() != std::noop_coroutine()) {
      active_handle().resume();

      if (state() == blocked_by::time && m_pending_delay > sleep_duration(0)) {
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
   * @param p_block_state - state that this context has been set to
   * @param p_block_info - information about the blocking conditions
   */
  void do_schedule(blocked_by p_block_state,
                   block_info p_block_info) noexcept override
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

  sleep_duration m_pending_delay{ 0 };
};

export class context_token
{
public:
  constexpr context_token() = default;
  constexpr context_token(context& p_capture) noexcept
    : m_context_address(std::bit_cast<uptr>(&p_capture))
  {
  }
  constexpr context_token& operator=(context& p_capture) noexcept
  {
    m_context_address = std::bit_cast<uptr>(&p_capture);
    return *this;
  }
  constexpr context_token& operator=(nullptr_t) noexcept
  {
    m_context_address = 0U;
    return *this;
  }

  constexpr context_token(context_token const& p_capture) noexcept = default;
  constexpr context_token& operator=(context_token const& p_capture) noexcept =
    default;
  constexpr context_token(context_token&& p_capture) noexcept = default;
  constexpr context_token& operator=(context_token& p_capture) noexcept =
    default;

  constexpr bool operator==(context& p_context) noexcept
  {
    return m_context_address == std::bit_cast<uptr>(&p_context);
  }

  [[nodiscard]] constexpr bool in_use() const noexcept
  {
    return m_context_address != 0U;
  }

  [[nodiscard]] auto address() const noexcept
  {
    return m_context_address != 0U;
  }

  [[nodiscard]] constexpr operator bool() const noexcept
  {
    return in_use();
  }

  // TODO(#29): Lease should return a guard variable that, on destruction,
  // unblocks and clear itself.
  constexpr void lease(context& p_capture) noexcept
  {
    m_context_address = std::bit_cast<uptr>(&p_capture);
  }

  constexpr std::suspend_always set_as_block_by_sync(context& p_capture)
  {
    if (in_use()) {
      auto* address = std::bit_cast<void*>(m_context_address);
      auto* inner_context = static_cast<context*>(address);
      p_capture.block_by_sync(inner_context);
    }
    return {};
  }

  constexpr void unblock_and_clear() noexcept
  {
    if (in_use()) {
      auto* address = std::bit_cast<void*>(m_context_address);
      auto* inner_context = static_cast<context*>(address);
      inner_context->unblock();
      m_context_address = 0U;
    }
  }

private:
  uptr m_context_address = 0U;
};

export struct io
{
  io(sleep_duration p_duration = sleep_duration{ 0u })
    : m_duration(p_duration)
  {
  }
  sleep_duration m_duration;
};

export struct sync
{
  sync(context_token p_context)
    : m_context(p_context)
  {
  }
  context_token m_context;
};

// =============================================================================
//
// Promise Base
//
// =============================================================================

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

  // Constructor for functions accepting no arguments
  promise_base(context& p_context)
    : m_context(&p_context)
  {
  }

  // Constructor for functions accepting arguments
  template<typename... Args>
  promise_base(context& p_context, Args&&...)
    : m_context(&p_context)
  {
  }

  // Constructor for member functions (handles 'this' parameter)
  template<typename Class>
  promise_base(Class&, context& p_context)
    : m_context(&p_context)
  {
  }

  // Constructor for member functions with additional parameters
  template<typename Class, typename... Args>
  promise_base(Class&, context& p_context, Args&&...)
    : m_context(&p_context)
  {
  }

  constexpr std::suspend_always initial_suspend() noexcept
  {
    return {};
  }

  template<typename Rep, typename Ratio>
  constexpr auto await_transform(
    std::chrono::duration<Rep, Ratio> p_sleep_duration) noexcept
  {
    return m_context->block_by_time(p_sleep_duration);
  }

  constexpr std::suspend_always await_transform() noexcept
  {
    m_context->block_by_io();
    return {};
  }

  template<typename U>
  constexpr U&& await_transform(U&& p_awaitable) noexcept
  {
    return static_cast<U&&>(p_awaitable);
  }

  constexpr auto& get_context()
  {
    return *m_context;
  }

  constexpr auto continuation()
  {
    return m_continuation;
  }

  constexpr void continuation(std::coroutine_handle<> p_continuation)
  {
    m_continuation = p_continuation;
  }

  constexpr std::coroutine_handle<> pop_active_coroutine()
  {
    m_context->active_handle(m_continuation);
    return m_continuation;
  }

protected:
  // Consider m_continuation as the return address of the coroutine. The
  // coroutine handle for the coroutine that called and awaited the future that
  // generated this promise is stored here.
  std::coroutine_handle<> m_continuation = std::noop_coroutine();
  class context* m_context;  // left uninitialized, compiler should warn me
};

export template<typename T>
class future;

template<typename T>
using monostate_or = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/**
 * @brief Represents a finished future of type void
 *
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

template<class Promise>
struct final_awaiter
{
  constexpr bool await_ready() noexcept
  {
    return false;
  }

  std::coroutine_handle<> await_suspend(
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

  void await_resume() noexcept
  {
  }
};

export template<typename T>
class promise : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using promise_base::operator delete;
  using our_handle = std::coroutine_handle<promise<T>>;

  friend class future<T>;

  constexpr final_awaiter<promise<T>> final_suspend() noexcept
  {
    return {};
  }

  void unhandled_exception() noexcept
  {
    *m_future_state = std::current_exception();
  }

  constexpr future<T> get_return_object() noexcept;

  template<typename U>
  void return_value(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    // set future to its awaited T value
    m_future_state->template emplace<T>(std::forward<U>(p_value));
  }

protected:
  future_state<T>* m_future_state;
};

export template<>
class promise<void> : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using promise_base::operator delete;
  using our_handle = std::coroutine_handle<promise<void>>;

  friend class future<void>;

  constexpr final_awaiter<promise<void>> final_suspend() noexcept
  {
    return {};
  }

  constexpr future<void> get_return_object() noexcept;

  void unhandled_exception() noexcept
  {
    *m_future_state = std::current_exception();
  }

  void return_void() noexcept
  {
    *m_future_state = std::monostate{};
  }

protected:
  future_state<void>* m_future_state;
};

export template<typename T>
class future
{
public:
  using promise_type = promise<T>;
  using handle_type = std::coroutine_handle<>;
  using full_handle_type = std::coroutine_handle<promise_type>;

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
   * @return true - operation is either finished
   * @return false - operation has yet to completed and does have a value.
   */
  [[nodiscard]] constexpr bool done() const
  {
    return not std::holds_alternative<handle_type>(m_state);
  }

  /**
   * @brief Reports if this async object has finished with an value
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

  [[nodiscard]] constexpr awaiter operator co_await() noexcept
  {
    return awaiter{ *this };
  }

  template<typename U>
  constexpr future(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    m_state.template emplace<T>(std::forward<U>(p_value));
  };

  future(future const& p_other) = delete;
  future& operator=(future const& p_other) = delete;

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

  void cancel()
  {
    // TODO(#37): consider if cancel should check the context state for blocked
    // by io or external and skip cancellation if thats the case.
    if (std::holds_alternative<handle_type>(m_state)) {
      std::get<handle_type>(m_state).destroy();
    }
    m_state = cancelled_state{};
  }

  bool is_cancelled()
  {
    return std::holds_alternative<cancelled_state>(m_state);
  }

  constexpr ~future()
  {
    if (std::holds_alternative<handle_type>(m_state)) {
      std::get<handle_type>(m_state).destroy();
    }
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
  }

  future_state<T> m_state{};
};

/**
 * @brief An async task is an async operation that performs some work but
 * doesn't return a value.
 *
 */
export using task = future<void>;

template<typename T>
constexpr future<T> promise<T>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<T>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  return future<T>{ handle };
}

constexpr future<void> promise<void>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<void>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  return future<void>{ handle };
}

void context::unsafe_cancel()
{
  // TODO(#38): Consider if a safe variant of cancel is achievable
  if (m_active_handle == std::noop_coroutine()) {
    return;
  }

  auto index = m_active_handle;

  while (true) {
    using base_handle = std::coroutine_handle<promise_base>;
    auto top = base_handle::from_address(index.address());
    auto continuation = top.promise().m_continuation;
    if (continuation == std::noop_coroutine()) {
      // We have found our top level coroutine
      top.destroy();
      m_stack_pointer = m_stack.data();
      return;
    }
    index = continuation;
  }
}
}  // namespace async::inline v0
