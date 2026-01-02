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

#include <cstddef>
#include <cstdint>

#include <chrono>
#include <coroutine>
#include <exception>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

export module async_context;

export import strong_ptr;

namespace async::inline v0 {

export using u8 = std::uint8_t;
export using byte = std::uint8_t;
export using usize = std::size_t;

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

// =============================================================================
//
// Context
//
// =============================================================================

template<typename T>
using monostate_or = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

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

struct noop_context_tag
{};

class promise_base;

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
    m_stack = { allocator.allocate_object<byte>(p_stack_size), p_stack_size };
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
    if (std::holds_alternative<blocked_by>(m_state)) {
      return std::get<blocked_by>(m_state);
    }
    return blocked_by::nothing;
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

  // TODO(#31): Implement! Doesn't do anything yet.
  void cancel()
  {
    while (m_active_handle != std::noop_coroutine()) {
      m_active_handle.destroy();
    }
    m_stack_index = 0;
  }

  [[nodiscard]] constexpr auto memory_used() const noexcept
  {
    return m_stack_index;
  }

  [[nodiscard]] constexpr auto capacity() const noexcept
  {
    return m_stack.size();
  }

  [[nodiscard]] constexpr auto memory_remaining() const noexcept
  {
    return capacity() - memory_used();
  }

  void rethrow_if_exception_caught()
  {
    if (std::holds_alternative<std::exception_ptr>(m_state)) [[unlikely]] {
      auto const exception_ptr_copy = std::get<std::exception_ptr>(m_state);
      m_state = 0uz;  // destroy exception_ptr and set state to `usize`
      std::rethrow_exception(exception_ptr_copy);
    }
  }

  [[nodiscard]] constexpr auto last_allocation_size() const noexcept
  {
    if (std::holds_alternative<usize>(m_state)) {
      return std::get<usize>(m_state);
    }
    return 0uz;
  }

  [[nodiscard]] constexpr bool is_proxy() const noexcept
  {
    return std::holds_alternative<proxy_info>(m_proxy);
  }

  context borrow_proxy() && = delete;
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
      allocator.deallocate_object<byte>(m_stack.data(), m_stack.size());
    }
  };

private:
  friend class promise_base;
  template<typename T>
  friend class future_promise_type;

  friend constexpr context& noop_context();

  struct proxy_info
  {
    context* origin = nullptr;
    context* parent = nullptr;
  };

  struct proxy_tag
  {};

  context(proxy_tag, context& p_parent)
    : m_active_handle(std::noop_coroutine())
    , m_state(0uz)
    , m_proxy(proxy_info{})
  {
    // We need to manually set:
    //    1. m_stack
    //    2. m_stack_index
    //    3. m_proxy

    auto const previous_stack = p_parent.m_stack;
    auto const previous_m_stack_index = p_parent.m_stack_index;
    auto const rest_of_the_stack =
      previous_stack.subspan(previous_m_stack_index);

    // Our proxy will take control over the rest of the unused stack memory from
    // the above context.
    m_stack = rest_of_the_stack;
    m_stack_index = 0uz;

    // Shrink the stack of the parent context to be equal to the current stack
    // index. This will prevent the parent context from being used again.
    p_parent.m_stack = p_parent.m_stack.first(previous_m_stack_index);

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

  context(noop_context_tag)
    : m_active_handle(std::noop_coroutine())
    , m_state(0uz)
    , m_proxy(noop_scheduler())
  {
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
    auto const new_stack_index = m_stack_index + p_bytes;
    if (new_stack_index > m_stack.size()) [[unlikely]] {
      throw bad_coroutine_alloc(this);
    }
    m_state = p_bytes;
    auto* const stack_address = &m_stack[m_stack_index];
    m_stack_index = new_stack_index;
    return stack_address;
  }

  constexpr void deallocate(std::size_t p_bytes)
  {
    m_stack_index -= p_bytes;
  }

  void set_exception(std::exception_ptr p_exception)
  {
    m_state = p_exception;
  }

  using context_state = std::variant<usize, blocked_by, std::exception_ptr>;
  using proxy_state = std::variant<proxy_info, mem::strong_ptr<scheduler>>;

  // Should stay close to a standard cache-line of 64 bytes (8 words).
  // Unfortunately we cannot achieve that if we want proxy support, so we must
  // deal with that by putting the scheduler towards the end since it is the
  // least hot part of the data.
  std::coroutine_handle<> m_active_handle = std::noop_coroutine();  // word 1
  std::span<byte> m_stack{};                                        // word 2-3
  usize m_stack_index{ 0uz };                                       // word 4
  context_state m_state{ 0uz };                                     // word 5-6
  proxy_state m_proxy{ proxy_info{} };                              // word 7-9
};

static_assert(sizeof(context) <=
              std::hardware_constructive_interference_size * 2);

export constexpr context& noop_context()
{
  static context noop(noop_context_tag{});
  return noop;
}

export class context_token
{
public:
  constexpr context_token() = default;
  constexpr context_token(context& p_capture) noexcept
    : m_context_address(std::bit_cast<std::uintptr_t>(&p_capture))
  {
  }
  constexpr context_token& operator=(context& p_capture) noexcept
  {
    m_context_address = std::bit_cast<std::uintptr_t>(&p_capture);
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
    return m_context_address == std::bit_cast<std::uintptr_t>(&p_context);
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
    m_context_address = std::bit_cast<std::uintptr_t>(&p_capture);
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
  std::uintptr_t m_context_address = 0U;
};

// =============================================================================
//
// Promise Base
//
// =============================================================================

/**
 * @brief
 *
 */
struct pop_active_coroutine
{};

class promise_base
{
public:
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

  // Add regular delete operators for normal coroutine destruction
  static constexpr void operator delete(void*) noexcept
  {
  }

  static constexpr void operator delete(void*, std::size_t) noexcept
  {
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

  constexpr auto await_transform(pop_active_coroutine) noexcept
  {
    return pop_active_coroutine();
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
  // Storage for the coroutine result/error
  std::coroutine_handle<> m_continuation = std::noop_coroutine();
  class context* m_context = nullptr;
};

export template<typename T>
class future;

export template<typename T>
class future_promise_type : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<future_promise_type<T>>;

  friend class future<T>;

  // Add regular delete operators for normal coroutine destruction
  static constexpr void operator delete(void*) noexcept
  {
  }

  static constexpr void operator delete(void*, std::size_t) noexcept
  {
  }

  void unhandled_exception() noexcept
  {
    pop_active_coroutine();
    // After this point accessing the state of the coroutine is UB.
    m_context->set_exception(std::current_exception());
  }

  struct final_awaiter
  {
    constexpr bool await_ready() noexcept
    {
      return false;
    }

    template<typename U>
    std::coroutine_handle<> await_suspend(
      std::coroutine_handle<future_promise_type<U>> p_self) noexcept
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
      return p_self.promise().pop_active_coroutine();
    }

    void await_resume() noexcept
    {
    }
  };

  constexpr final_awaiter final_suspend() noexcept
  {
    return {};
  }

  constexpr future<T> get_return_object() noexcept;

  template<typename U>
  void return_value(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    m_value_address->emplace(std::forward<U>(p_value));
  }

  ~future_promise_type()
  {
    m_context->deallocate(m_frame_size);
  }

private:
  std::optional<T>* m_value_address;
  usize m_frame_size;
};

export template<>
class future_promise_type<void> : public promise_base
{
public:
  // Inherit constructors & operator new
  using promise_base::promise_base;
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<future_promise_type<void>>;
  friend class future<void>;

  future_promise_type();

  constexpr void return_void() noexcept
  {
    // Do nothing
  }

  constexpr future<void> get_return_object() noexcept;

  // Delete operators are defined as no-ops to ensure that these calls get
  // removed from the binary if inlined.
  static constexpr void operator delete(void*) noexcept
  {
  }
  static constexpr void operator delete(void*, std::size_t) noexcept
  {
  }

  struct final_awaiter
  {
    constexpr bool await_ready() noexcept
    {
      return false;
    }

    std::coroutine_handle<> await_suspend(
      std::coroutine_handle<future_promise_type<void>> p_self) noexcept
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
      return p_self.promise().pop_active_coroutine();
    }

    constexpr void await_resume() noexcept
    {
    }
  };

  constexpr final_awaiter final_suspend() noexcept
  {
    return {};
  }

  void unhandled_exception() noexcept
  {
    m_context->set_exception(std::current_exception());
  }

  ~future_promise_type()
  {
    m_context->deallocate(m_frame_size);
  }

private:
  usize m_frame_size = 0;
};

// TODO(kammce): specialize against future<void> to eliminate optional member
// variable
export template<typename T>
class future
{
public:
  using promise_type = future_promise_type<T>;
  friend promise_type;
  using handle_type = std::coroutine_handle<>;
  using future_handle_type = std::coroutine_handle<promise_type>;

  [[nodiscard]] constexpr auto handle() const
  {
    return future_handle_type::from_address(m_handle.address());
  }

  constexpr void resume() const
  {
    handle().promise().get_context().active_handle().resume();
  }

  /**
   * @brief Reports if this async object has finished its operation and now
   * contains a value.
   *
   * @return true - operation finished and the response can be acquired by
   * `result()`
   * @return false - operation has yet to completed and does have a value.
   */
  [[nodiscard]] constexpr bool done() const
  {
    // True if the handle isn't valid
    // OR
    // If the coroutine is valid, then check if it has suspended at its final
    // suspension point.
    return m_result.value();
  }

  /**
   * @brief Extract result value from async operation.
   *
   * The result is undefined if `done()` does not return `true`.
   *
   * @return Type - reference to the value from this async operation.
   */
  [[nodiscard]] constexpr std::optional<T>& result()
    requires(not std::is_void_v<T>)
  {
    return m_result;
  }

  // Awaiter for when this task is awaited
  struct awaiter
  {
    future<T>* m_operation;

    constexpr explicit awaiter(future<T>* p_operation) noexcept
      : m_operation(p_operation)
    {
    }

    [[nodiscard]] constexpr bool await_ready() const noexcept
    {
      return m_operation->done();
    }

    // TODO(): make this actually typed to future promise
    std::coroutine_handle<> await_suspend(
      handle_type p_calling_coroutine) noexcept
    {
      m_operation->handle().promise().continuation(p_calling_coroutine);
      return m_operation->m_handle;
    }

    constexpr monostate_or<T>& await_resume() const
    {
      m_operation->handle()
        .promise()
        .get_context()
        .rethrow_if_exception_caught();

      // ⚠️ unchecked optional access for performance
      return *m_operation->m_result;
    }
  };

  [[nodiscard]] constexpr awaiter operator co_await() noexcept
  {
    return awaiter{ this };
  }

  // Run synchronously and return result
  monostate_or<T>& sync_wait()
  {
    // Perform await operation manually and synchonously
    auto manual_awaiter = awaiter{ this };

    // Check if our awaiter is not ready and if so, run it until it finishes
    if (not manual_awaiter.await_ready()) {
      auto& context = handle().promise().get_context();
      context.sync_wait();
    }

    // This thread of execution has completed now we can return the result
    return manual_awaiter.await_resume();
  }

  constexpr future() noexcept
    requires(std::is_void_v<T>)
    : m_result(monostate_or<T>{})
  {
  }

  template<typename U>
  constexpr future(U&& p_value) noexcept
    requires(not std::is_void_v<T>)
  {
    m_handle = std::noop_coroutine();
    m_result.emplace(std::forward<U>(p_value));
  };

  future(future const& p_other) = delete;
  future& operator=(future const& p_other) = delete;

  constexpr future(future&& p_other) noexcept
    : m_handle(p_other.m_handle)
  {
    if constexpr (not std::is_void_v<T>) {
      handle().promise().m_value_address = &m_result;
    }
  }

  constexpr future& operator=(future&& p_other) noexcept
  {
    if (this != &p_other) {
      m_handle = p_other.m_handle;
      if constexpr (not std::is_void_v<T>) {
        handle().promise().m_value_address = &m_result;
      }
    }
    return *this;
  }

  void cancel()
  {
    // TODO(#31): Implement! Properly
    if (m_handle != std::noop_coroutine() && not m_handle.done()) {
#if 1
      auto& context = handle().promise().get_context();
      auto active_handle = context.active_handle();
      while (m_handle != active_handle) {
        // This can only be a future<T> or noop_coroutine
        future_handle_type::from_address(active_handle.address())
          .promise()
          .pop_active_coroutine();
        active_handle.destroy();
        // Get the next active coroutine handle
        active_handle = context.active_handle();
      }
      m_handle.destroy();
#endif
    }
  }

  constexpr ~future()
  {
    cancel();
  }

private:
  friend promise_type;

  explicit constexpr future(future_handle_type p_handle)
    : m_handle(p_handle)
  {
    auto& promise = p_handle.promise();
    if constexpr (not std::is_void_v<T>) {
      promise.m_value_address = &m_result;
    }
  }

  handle_type m_handle = std::noop_coroutine();
  std::optional<monostate_or<T>> m_result;
};

template<typename T>
constexpr future<T> future_promise_type<T>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<future_promise_type<T>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  // Retrieve the frame size and store it for deallocation on destruction
  m_frame_size = m_context->last_allocation_size();
  return future<T>{ handle };
}

export template<>
class future<void>
{
public:
  using promise_type = future_promise_type<void>;
  friend promise_type;
  using handle_type = std::coroutine_handle<>;
  using future_handle_type = std::coroutine_handle<promise_type>;

  [[nodiscard]] constexpr auto handle() const
  {
    return future_handle_type::from_address(m_handle.address());
  }

  constexpr void resume() const
  {
    handle().promise().get_context().active_handle().resume();
  }

  /**
   * @brief Reports if this async object has finished its operation and now
   * contains a value.
   *
   * @return true - operation finished and the response can be acquired by
   * `result()`
   * @return false - operation has yet to completed and does have a value.
   */
  [[nodiscard]] constexpr bool done() const
  {
    // True if the handle isn't valid
    // OR
    // If the coroutine is valid, then check if it has suspended at its final
    // suspension point.
    return m_handle == std::noop_coroutine() || m_handle.done();
  }

  // Awaiter for when this task is awaited
  struct awaiter
  {
    future<void>* m_operation;

    constexpr explicit awaiter(future<void>* p_operation) noexcept
      : m_operation(p_operation)
    {
    }

    [[nodiscard]] constexpr bool await_ready() const noexcept
    {
      return m_operation->done();
    }

    // TODO(): make this actually typed to future promise
    std::coroutine_handle<> await_suspend(
      handle_type p_calling_coroutine) noexcept
    {
      m_operation->handle().promise().continuation(p_calling_coroutine);
      return m_operation->m_handle;
    }

    constexpr void await_resume() const
    {
      m_operation->handle()
        .promise()
        .get_context()
        .rethrow_if_exception_caught();
    }
  };

  [[nodiscard]] constexpr awaiter operator co_await() noexcept
  {
    return awaiter{ this };
  }

  // Run synchronously and return result
  void sync_wait()
  {
    // Perform await operation manually and synchonously
    auto manual_awaiter = awaiter{ this };

    // Check if our awaiter is not ready and if so, run it until it finishes
    if (not manual_awaiter.await_ready()) {
      auto& context = handle().promise().get_context();
      context.sync_wait();
    }

    // This thread of execution has completed now we can return the result
    manual_awaiter.await_resume();
  }

  future(future const& p_other) = delete;
  future& operator=(future const& p_other) = delete;

  constexpr future(future&& p_other) noexcept
    : m_handle(p_other.m_handle)
  {
    p_other.m_handle = std::noop_coroutine();
  }

  constexpr future& operator=(future&& p_other) noexcept
  {
    if (this != &p_other) {
      m_handle = p_other.m_handle;
      p_other.m_handle = std::noop_coroutine();
    }
    return *this;
  }

  void cancel()
  {
    // TODO(#31): Implement! Properly
    if (not done()) {
#if 1
      auto& context = handle().promise().get_context();
      auto active_handle = context.active_handle();
      // Keep destroying coroutines until we find our handle
      while (m_handle != active_handle) {
        future_handle_type::from_address(active_handle.address())
          .promise()
          .pop_active_coroutine();
        active_handle.destroy();
        // Get the next active coroutine handle
        active_handle = context.active_handle();
      }
      m_handle.destroy();
#endif
    }
  }

  constexpr ~future()
  {
    cancel();
  }

private:
  friend promise_type;

  explicit constexpr future(handle_type p_handle)
    : m_handle(p_handle)
  {
  }

  handle_type m_handle = std::noop_coroutine();
};

inline constexpr future<void>
future_promise_type<void>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<future_promise_type<void>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  // Retrieve the frame size and store it for deallocation on destruction
  m_frame_size = m_context->last_allocation_size();
  return future<void>{ handle };
}

}  // namespace async::inline v0
