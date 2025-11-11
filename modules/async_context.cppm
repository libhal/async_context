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
#include <functional>
#include <memory_resource>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

export module async_context;

namespace async::inline v0 {

using u8 = std::uint8_t;
using byte = std::uint8_t;
using usize = std::size_t;

enum class blocked_by : u8
{
  /// Not blocked by anything, ready to run!
  nothing = 0,
  /// Blocked by an amount of time that must be waited before the task resumes.
  /// It is the responsibility of the scheduler to defer calling the active
  /// coroutine of an `context` until the amount of time requested has
  /// elapsed.
  ///
  /// The time duration passed to the transition handler function represents the
  /// amount of time that the coroutine must suspend for until before scheduling
  /// the task again. Timed delays in this fashion are not real time and only
  /// represent the shortest duration of time necessary to fulfil the
  /// coroutine's delay needs. To schedule the coroutine before its delay time
  /// has been awaited, is considered to be undefined behavior. It is the
  /// responsibility of the develop of the transition handler to ensure that
  /// tasks are not executed until their delay time has elapsed.
  ///
  /// A value of 0 means do not wait and suspend but set the blocked by state to
  /// `time`. This will suspend the coroutine and it later. This is equivalent
  /// to just performing `std::suspend_always` but with additional steps, thus
  /// it is not advised to perform `co_await 0ns`.
  time = 1,
  /// Blocked by an I/O operation such as a DMA transfer or a bus. It is the
  /// responsibility of an interrupt (or thread performing I/O operations) to
  /// change the state to 'nothing' when the operation has completed.
  ///
  /// The time duration passed to the transition handler represents the amount
  /// of time the task must wait until it may resume the task even before its
  /// block status has transitioned to "nothing". This would represent the
  /// coroutine providing a hint to the scheduler about polling the coroutine. A
  /// value of 0 means wait indefinitely.
  io = 2,
  /// Blocked due to a resource being unavailable
  sync = 3,
};

class context;

/**
 * @brief Thrown when an async::context runs out of stack memory
 *
 * This occurs if a coroutine co_awaits a function and the coroutine promise
 * cannot fit withint he context.
 *
 */
struct bad_coroutine_alloc : std::bad_alloc
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

// TODO(#9): Replace std::function with stdex::inplace_function
using transition_handler = std::function<
  void(context&, blocked_by, std::variant<sleep_duration, context*>)>;

class context
{
public:
  static auto constexpr default_timeout = sleep_duration(0);

  /**
   * @brief Default construct a new async context object
   *
   * Using this async context on a coroutine will result in the allocation
   * throwing an exception.
   */
  context() = default;

  void unblock()
  {
    transition_to(blocked_by::nothing, default_timeout);
  }
  void unblock_without_notification()
  {
    m_state = blocked_by::nothing;
  }
  void block_by_time(sleep_duration p_duration)
  {
    transition_to(blocked_by::time, p_duration);
  }
  void block_by_io(sleep_duration p_duration = default_timeout)
  {
    transition_to(blocked_by::io, p_duration);
  }
  void block_by_sync(sleep_duration p_duration = default_timeout)
  {
    transition_to(blocked_by::sync, p_duration);
  }

  [[nodiscard]] constexpr std::coroutine_handle<> active_handle() const
  {
    return m_active_handle;
  }

  [[nodiscard]] auto state() const
  {
    return std::get<1>(m_state);
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

  constexpr auto memory_used()
  {
    return m_stack_index;
  }

  constexpr auto capacity()
  {
    return m_stack.size();
  }

  constexpr auto memory_remaining()
  {
    return capacity() - memory_used();
  }

  /**
   * @brief Construct a new context object
   *
   * @param p_transition_handler - a pointer to a transition handler that
   * handles transitions in blocked_by state. The transitions handler must
   * outlive the lifetime of this object.
   * @param p_stack_memory - span to a block of memory reserved for this context
   * to be used as stack memory for coroutine persistent memory. This buffer
   * must outlive the lifetime of this object.
   */
  explicit context(transition_handler* p_transition_handler,
                   std::span<byte> p_stack_memory)
    : m_transition_handler(p_transition_handler)
    , m_stack(p_stack_memory)
  {
  }

  void rethrow_if_exception_caught()
  {
    if (std::holds_alternative<std::exception_ptr>(m_state)) [[unlikely]] {
      auto const exception_ptr_copy = std::get<std::exception_ptr>(m_state);
      m_state = 0uz;  // destroy exception_ptr and set state to `usize`
      std::rethrow_exception(exception_ptr_copy);
    }
  }

  constexpr auto last_allocation_size()
  {
    return std::get<usize>(m_state);
  }

  void transition_to(blocked_by p_new_state, sleep_duration p_info)
  {
    m_state = p_new_state;
    if (m_transition_handler) {
      (*m_transition_handler)(*this, p_new_state, p_info);
    }
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

private:
  friend class promise_base;

  // Should stay within a cache-line of 64 bytes (8 words) on 64-bit systems
  std::coroutine_handle<> m_active_handle = std::noop_coroutine();  // word 1
  std::span<byte> m_stack{};                                        // word 3-4
  usize m_stack_index = 0;                                          // word 5
  std::variant<usize, blocked_by, std::exception_ptr> m_state;      // word 6-7
  transition_handler m_transition_handler = nullptr;                // word 2
};

static_assert(sizeof(context) <=
              std::hardware_constructive_interference_size * 2);

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
    m_context->block_by_time(p_sleep_duration);
    return std::suspend_always{};
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

  constexpr auto& context()
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
  class context* m_context;
};

template<typename T>
class future;

template<typename T>
class future_promise_type : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<future_promise_type<T>>;

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
    m_value_address->template emplace<T>(std::forward<U>(p_value));
    self_destruct();
  }

  void set_object_address(
    std::variant<monostate_or<T>, our_handle>* p_value_address)
  {
    m_value_address = p_value_address;
  }

  void self_destruct()
  {
    auto* const context = m_context;
    auto handle = our_handle::from_promise(*this);
    handle.destroy();
    context->deallocate(m_frame_size);
  }

private:
  std::variant<monostate_or<T>, std::coroutine_handle<future_promise_type<T>>>*
    m_value_address;
  usize m_frame_size;
};

template<>
class future_promise_type<void> : public promise_base
{
public:
  // Inherit constructors & operator new
  using promise_base::promise_base;
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<future_promise_type<void>>;

  future_promise_type();

  void set_object_address(
    std::variant<monostate_or<void>, our_handle>* p_value_address)
  {
    m_value_address = p_value_address;
  }

  constexpr void return_void() noexcept
  {
    *m_value_address = std::monostate{};
    self_destruct();
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

  void self_destruct()
  {
    auto* const context = m_context;
    auto handle = our_handle::from_promise(*this);
    handle.destroy();
    context->deallocate(m_frame_size);
  }

private:
  std::variant<monostate_or<void>, our_handle>* m_value_address = nullptr;
  usize m_frame_size = 0;
};

template<typename T>
class future
{
public:
  using promise_type = future_promise_type<T>;
  friend promise_type;
  using task_handle_type = std::coroutine_handle<promise_type>;

  constexpr void resume() const
  {
    auto active = handle().promise().context().active_handle();
    active.resume();
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
    return std::holds_alternative<monostate_or<T>>(m_result);
  }

  /**
   * @brief Extract result value from async operation.
   *
   * The result is undefined if `done()` does not return `true`.
   *
   * @return Type - reference to the value from this async operation.
   */
  [[nodiscard]] constexpr monostate_or<T>& result()
  {
    return std::get<monostate_or<T>>(m_result);
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

    // Generic await_suspend for any promise type
    template<typename Promise>
    std::coroutine_handle<> await_suspend(
      std::coroutine_handle<Promise> p_continuation) noexcept
    {
      m_operation->handle().promise().continuation(p_continuation);
      return m_operation->handle();
    }

    constexpr monostate_or<T>& await_resume() const
    {
      // If the async object is being resumed and it has not destroyed itself
      // and been replaced with the result value, then there MUST be an
      // exception that needs to be propagated through the calling coroutine.
      if (std::holds_alternative<task_handle_type>(m_operation->m_result))
        [[unlikely]] {
        auto& context = m_operation->handle().promise().context();

        context.rethrow_if_exception_caught();

        /// NOTE: If this await_resume() is called via `co_await`, then the
        /// resources of the call will be cleaned up when then future is
        /// destroyed.
      }
      return m_operation->result();
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
      auto& context = handle().promise().context();
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
    m_result.template emplace<T>(std::forward<U>(p_value));
  };

  future(future const& p_other) = delete;
  future& operator=(future const& p_other) = delete;

  constexpr future(future&& p_other) noexcept
    : m_result(std::exchange(p_other.m_result, {}))
  {
    if (std::holds_alternative<task_handle_type>(m_result)) {
      handle().promise().set_object_address(&m_result);
    }
  }

  constexpr future& operator=(future&& p_other) noexcept
  {
    if (this != &p_other) {
      m_result = std::exchange(p_other.m_result, {});
      if (std::holds_alternative<task_handle_type>(m_result)) {
        handle().promise().set_object_address(&m_result);
      }
    }
    return *this;
  }

  constexpr ~future()
  {
    if (std::holds_alternative<task_handle_type>(m_result)) {
      /// NOTE: This only occurs if the future was not completed before its
      /// future object was destroyed.
      handle().promise().self_destruct();
    }
  }

  [[nodiscard]] auto handle() const
  {
    return std::get<task_handle_type>(m_result);
  }

  void set_context(context& p_context)
  {
    handle().promise().context() = p_context;
  }

private:
  friend promise_type;

  explicit constexpr future(task_handle_type p_handle)
    : m_result(p_handle)
  {
    auto& promise = p_handle.promise();
    promise.set_object_address(&m_result);
  }

  std::variant<monostate_or<T>, task_handle_type> m_result;
};

template<typename T>
constexpr future<T> future_promise_type<T>::get_return_object() noexcept
{
  auto handle =
    std::coroutine_handle<future_promise_type<T>>::from_promise(*this);
  m_context->active_handle(handle);
  // Copy the last allocation size before changing the representation of
  // m_state to 'blocked_by::nothing'.
  m_frame_size = m_context->last_allocation_size();
  // Now stomp the union out and set it to the blocked_by::nothing state.
  m_context->unblock_without_notification();
  return future<T>{ handle };
}

inline constexpr future<void>
future_promise_type<void>::get_return_object() noexcept
{
  auto handle =
    std::coroutine_handle<future_promise_type<void>>::from_promise(*this);
  m_context->active_handle(handle);
  // Copy the last allocation size before changing the representation of
  // m_state to 'blocked_by::nothing'.
  m_frame_size = m_context->last_allocation_size();
  // Now stomp the union out and set it to the blocked_by::nothing state.
  m_context->unblock_without_notification();
  return future<void>{ handle };
}

}  // namespace async::inline v0
