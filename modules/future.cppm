module;

#include <bitset>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <memory_resource>
#include <new>
#include <numeric>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

module async_context:future;

import :context;
import :promise_base;

export namespace async::inline v0 {
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
    m_context->m_state = std::current_exception();
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
    m_context->m_state = std::current_exception();
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
  m_context->m_state = blocked_by(blocked_by::nothing);
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
  m_context->m_state = blocked_by::nothing;
  return future<void>{ handle };
}
}  // namespace async::inline v0
