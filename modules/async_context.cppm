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
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#if DEBUGGING
#include <print>
#endif

export module async_context;

export import strong_ptr;

namespace async::inline v0 {

export using u8 = std::uint8_t;
export using byte = std::uint8_t;
export using usize = std::size_t;
export using uptr = std::uintptr_t;

export enum class blocked_by : u8 {
  /// Not blocked by anything, ready to run!
  nothing = 0,

  /// Blocked by a time duration that must elapse before resuming.
  time = 1,

  /// Blocked by an I/O operation (DMA, bus transaction, etc.).
  /// An interrupt or I/O completion will call unblock() when ready.
  io = 2,

  /// Blocked by a synchronization primitive or resource contention.
  /// Examples: mutex, semaphore, two coroutines competing for an I2C bus.
  /// The transition handler may integrate with OS schedulers or implement
  /// priority inheritance strategies.
  sync = 3,

  /// Blocked by an external coroutine outside the async::context system.
  /// Examples: co_awaiting std::task, std::generator, or third-party async
  /// types. The transition handler has no control over scheduling - it can only
  /// wait for the external coroutine's await_resume() to call unblock().
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

export class operation_cancelled : public std::exception
{
  [[nodiscard]] char const* what() const noexcept override
  {
    return "An async::context ran out of memory!";
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
using sleep_duration = std::chrono::nanoseconds;

/**
 * @brief
 *
 */
export class scheduler
{
public:
  using block_info = std::variant<std::monostate, sleep_duration, context*>;

  /**
   * @brief
   *
   * It is up to the scheduler to ensure that concurrent calls to this API are
   * serialized appropriately. For a single threaded event loop, syncronization
   * and serialization is not necessary. For a thread pool implementation,
   * syncronization nd serialization must be considered.
   *
   * @param p_context - the context that is requested to be scheduled
   * @param p_block_state - the type of blocking event the context has
   * encountered.
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
  void schedule(context& p_context,
                blocked_by p_block_state,
                block_info p_block_info) noexcept
  {
    return do_schedule(p_context, p_block_state, p_block_info);
  }

  /**
   * @brief Get allocator from scheduler
   *
   * The memory_resource returned be owned or embedded within the scheduler. The
   * memory_resource and its backing memory must live as long as the scheduler.
   * The returned reference MUST NOT be bound to a nullptr.
   *
   * @return std::pmr::memory_resource& - the memory resource to be used to
   * allocate memory for async::context stack memory. The memory_resource must
   * be owned or embedded within the scheduler.
   */
  std::pmr::memory_resource& get_allocator() noexcept
  {
    return do_get_allocator();
  }

private:
  virtual void do_schedule(context& p_context,
                           blocked_by p_block_state,
                           block_info p_block_info) noexcept = 0;

  virtual std::pmr::memory_resource& do_get_allocator() noexcept = 0;
};

export constexpr mem::strong_ptr<scheduler> noop_scheduler()
{
  struct noop_scheduler : scheduler
  {
    void do_schedule(context&, blocked_by, block_info) noexcept override
    {
      return;
    }

    std::pmr::memory_resource& do_get_allocator() noexcept override
    {
      std::terminate();
    }
  };

  static noop_scheduler sched;

  return mem::strong_ptr(mem::unsafe_assume_static_tag{}, sched);
}

class promise_base;

using word_t = std::max_align_t;
static constexpr usize word_size = sizeof(word_t);
static constexpr usize word_shift = std::countr_zero(word_size);

export class context
{
public:
  static auto constexpr default_timeout = sleep_duration(0);
  using scheduler_t = mem::strong_ptr<scheduler>;

  // with something thats easier and safer to work with.
  /**
   * @brief Construct a new context object
   *
   * @param p_scheduler - a pointer to a transition handler that
   * handles transitions in blocked_by state.
   * @param p_stack_size - Number of bytes to allocate for the context's stack
   * memory.
   */
  context(scheduler_t const& p_scheduler, usize p_stack_size)
    : m_proxy(p_scheduler)
  {
    using poly_allocator = std::pmr::polymorphic_allocator<byte>;
    auto allocator = poly_allocator(&p_scheduler->get_allocator());

    // Allocate memory for stack and assign to m_stack
    m_stack = { allocator.allocate_object<uptr>(p_stack_size),
                1 + (p_stack_size >> word_shift) };
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

  void sync_wait()
  {
    while (m_active_handle != std::noop_coroutine()) {
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

  [[nodiscard]] constexpr bool is_proxy() const noexcept
  {
    return std::holds_alternative<proxy_info>(m_proxy);
  }

  /**
   * @brief Prevent a temporary context from being borrowed
   *
   * Required to prevent proxies with dangling references to a context
   *
   * @return context - (never returns generates compile time error)
   */
  context borrow_proxy() && = delete;

  /**
   * @brief
   *
   * @return context
   */
  context borrow_proxy() &
  {
    return { proxy_tag{}, *this };
  }

  ~context()
  {
    // We need to destroy the entire coroutine chain here!
    // cancel();

    if (is_proxy()) {
      auto* parent = std::get<proxy_info>(m_proxy).parent;
      // Unshrink parent stack, by setting its range to be the start of its
      // stack and the end to be the end of this stack.
      parent->m_stack = std::span(parent->m_stack.begin(), m_stack.end());
    } else {
      using poly_allocator = std::pmr::polymorphic_allocator<byte>;
      auto scheduler = std::get<scheduler_t>(m_proxy);
      auto allocator = poly_allocator(&scheduler->get_allocator());
      allocator.deallocate_object<uptr>(m_stack.data(), m_stack.size());
    }
  };

private:
  friend class promise_base;
  template<typename T>
  friend class promise;

  struct proxy_info
  {
    context* origin = nullptr;
    context* parent = nullptr;
  };

  struct proxy_tag
  {};

  context(proxy_tag, context& p_parent)
    : m_active_handle(std::noop_coroutine())
    , m_proxy(proxy_info{})
  {
    // We need to manually set:
    //    1. m_stack
    //    2. m_stack_index
    //    3. m_proxy

    auto const previous_stack = p_parent.m_stack;
    auto const previous_m_stack_pointer = p_parent.m_stack_pointer;
    auto const rest_of_the_stack =
      std::span(previous_m_stack_pointer,
                previous_m_stack_pointer - previous_stack.data());

    // Our proxy will take control over the rest of the unused stack memory from
    // the above context.
    m_stack = rest_of_the_stack;
    m_stack_pointer = m_stack.data();

    // Shrink the stack of the parent context to be equal to the current stack
    // index. This will prevent the parent context from being used again.
    p_parent.m_stack =
      std::span(p_parent.m_stack.data(), previous_m_stack_pointer);

    // If this is a proxy, take its pointer to the origin
    if (p_parent.is_proxy()) {
      auto info = std::get<proxy_info>(p_parent.m_proxy);
      m_proxy = proxy_info{
        .origin = info.origin,
        .parent = &p_parent,
      };
    } else {  // Otherwise, the current parent is the origin.
      m_proxy = proxy_info{
        .origin = &p_parent,
        .parent = &p_parent,
      };
    }
  }

  [[nodiscard]] constexpr context* origin() noexcept
  {
    if (is_proxy()) {
      return std::get<proxy_info>(m_proxy).origin;
    }
    return this;
  }

  [[nodiscard]] constexpr context const* origin() const noexcept
  {
    if (is_proxy()) {
      return std::get<proxy_info>(m_proxy).origin;
    }
    return this;
  }

  constexpr void transition_to(
    blocked_by p_new_state,
    scheduler::block_info p_info = std::monostate{}) noexcept
  {
    auto* origin_ptr = origin();
    origin_ptr->m_state = p_new_state;
    std::get<scheduler_t>(origin_ptr->m_proxy)
      ->schedule(*origin_ptr, p_new_state, p_info);
  }

  [[nodiscard]] constexpr void* allocate(std::size_t p_bytes)
  {
    constexpr size_t mask = sizeof(uptr) - 1uz;
    constexpr size_t shift = std::countr_zero(sizeof(uptr));

    // The extra 1 word is for the stack pointer's address
    size_t const words_needed = 1uz + ((p_bytes + mask) >> shift);
    auto const new_stack_index = m_stack_pointer + words_needed;

    if (new_stack_index > m_stack.end().base()) [[unlikely]] {
      throw bad_coroutine_alloc(this);
    }

    // Put the address of the stack pointer member on the stack, before the
    // coroutine frame, such that the delete operation can find the address and
    // update it.
    *m_stack_pointer = std::bit_cast<uptr>(&m_stack_pointer);
#if DEBUGGING
    std::println("💾 Allocating {} words, current stack {}, new stack {}, "
                 "stack pointer member address: 0x{:x}",
                 words_needed,
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

  using proxy_state = std::variant<proxy_info, mem::strong_ptr<scheduler>>;

  // Should stay close to a standard cache-line of 64 bytes (8 words).
  // Unfortunately we cannot achieve that if we want proxy support, so we must
  // deal with that by putting the scheduler towards the end since it is the
  // least hot part of the data.
  std::coroutine_handle<> m_active_handle = std::noop_coroutine();  // word 1
  std::span<uptr> m_stack{};                                        // word 2-3
  uptr* m_stack_pointer = nullptr;                                  // word 4
  blocked_by m_state = blocked_by::nothing;                         // word 5
  proxy_state m_proxy{};                                            // word 6-7
};

static_assert(sizeof(context) <= std::hardware_constructive_interference_size);

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
    // Acquire the pointer to the context stack memory from behind the promise
    // memory.
    auto** stack_pointer_address = *(static_cast<uptr***>(p_promise) - 1);
    // Update the stack pointer's address to be equal where it was before the
    // promise was allocated. Or said another way, the
#if DEBUGGING
    std::println(
      "Deleting {}, context's stack address ptr is at {}. Moving stack "
      "pointer to = {}, stack pointer was previously = {}",
      p_promise,
      static_cast<void*>(stack_pointer_address),
      static_cast<void*>(static_cast<uptr*>(p_promise) - 1),
      static_cast<void*>(*stack_pointer_address));
#endif
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
        .active_handle()
        .resume();
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
   * @brief Extract result value from async operation.
   *
   * Throws std::bad_variant_access if `done()` return false or `cancelled()`
   * return true.
   *
   * @return Type - reference to the value from this async operation.
   */
  [[nodiscard]] constexpr monostate_or<T>& result()
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
      return m_operation.m_state.index() >= 1;
    }

    std::coroutine_handle<> await_suspend(
      full_handle_type p_calling_coroutine) noexcept
    {
      // This will not throw because the discriminate check was performed in
      // `await_ready()` via the done() function. `done()` checks if the state
      // is `handle_type` and if it is, it returns false, causing the code to
      // call await_suspend().
      auto handle = std::get<handle_type>(m_operation.m_state);
      full_handle_type::from_address(handle.address())
        .promise()
        .continuation(p_calling_coroutine);
      return handle;
    }

    constexpr monostate_or<T>& await_resume() const
      requires(not std::is_void_v<T>)
    {
      // Combined with await_ready's `>= 1`, this becomes `== 1`
      if (m_operation.m_state.index() < 2) [[likely]] {
        return *std::get_if<1>(&m_operation.m_state);
      }
      // index >= 2, error territory
      if (m_operation.m_state.index() == 3) {
        std::rethrow_exception(*std::get_if<3>(&m_operation.m_state));
      }
      throw operation_cancelled{};
    }

    constexpr void await_resume() const
      requires(std::is_void_v<T>)
    {
      if (m_operation.m_state.index() < 2) [[likely]] {
        return;
      }
      if (m_operation.m_state.index() == 3) {
        std::rethrow_exception(*std::get_if<3>(&m_operation.m_state));
      }
      throw operation_cancelled{};
    }
  };

  [[nodiscard]] constexpr awaiter operator co_await() noexcept
  {
    return awaiter{ *this };
  }

  /**
   * @brief Run future synchronously until the future is done
   *
   */
  void sync_wait()
    requires(std::is_void_v<T>)
  {
    while (not done()) {
      resume();
    }

    if (auto* ex = std::get_if<std::exception_ptr>(&m_state)) [[unlikely]] {
      std::rethrow_exception(*ex);
    }
  }

  /**
   * @brief Run synchronously until the future is done and return its result
   *
   * @returns monostate_or<T> - Returns a reference to contained object
   */
  monostate_or<T>& sync_wait()
    requires(not std::is_void_v<T>)
  {
    while (not done()) {
      resume();
    }

    if (auto* ex = std::get_if<std::exception_ptr>(&m_state)) [[unlikely]] {
      std::rethrow_exception(*ex);
    }

    return std::get<T>(m_state);
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

template<typename T>
constexpr future<T> promise<T>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<T>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  return future<T>{ handle };
}

inline constexpr future<void> promise<void>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<void>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  return future<void>{ handle };
}

void context::unsafe_cancel()
{
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
