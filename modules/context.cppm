module;

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

module async_context:context;

export import :types;

export namespace async::inline v0 {

class context;

template<typename T>
using monostate_or = std::conditional_t<std::is_void_v<T>, std::monostate, T>;

/**
 * @brief The data type for sleep time duration
 *
 */
using sleep_duration = std::chrono::nanoseconds;

// TODO(#9): Replace std::function with stdex::inplace_function
using transition_handler =
  std::function<void(context&, blocked_by, sleep_duration)>;

class runtime_base
{
public:
  ~runtime_base()
  {
    if (m_resource) {
      std::pmr::polymorphic_allocator<byte>(m_resource)
        .deallocate(m_stack.data(), m_stack.size());
    }
  }

  runtime_base(runtime_base const&) = delete;
  runtime_base& operator=(runtime_base const&) = delete;
  runtime_base(runtime_base&&) = default;
  runtime_base& operator=(runtime_base&&) = default;

  void release_context(usize idx)
  {
    if (m_release) {
      m_release(this, idx);
    }
  }

protected:
  using release_function = void(runtime_base*, usize);

  explicit runtime_base(std::pmr::memory_resource& p_resource,
                        usize p_coroutine_stack_size,
                        transition_handler p_handler,
                        release_function* p_release_function)
    : m_resource(&p_resource)
    , m_handler(std::move(p_handler))
    , m_release(p_release_function)
  {
    m_stack = std::span{
      std::pmr::polymorphic_allocator<byte>(m_resource)
        .allocate(p_coroutine_stack_size),
      p_coroutine_stack_size,
    };
  }

  friend class context;

  // Should stay within a cache-line of 64 bytes (8 words) on 64-bit systems
  std::pmr::memory_resource* m_resource = nullptr;  // word 1
  std::span<byte> m_stack{};                        // word 2-3
  transition_handler m_handler;                     // word 4-7
  release_function* m_release = nullptr;            // word 8
};

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
  void block_by_inbox_empty(sleep_duration p_duration = default_timeout)
  {
    transition_to(blocked_by::inbox_empty, p_duration);
  }
  void block_by_outbox_full(sleep_duration p_duration = default_timeout)
  {
    transition_to(blocked_by::outbox_full, p_duration);
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

  void rethrow_if_exception_caught()
  {
    if (std::holds_alternative<std::exception_ptr>(m_state)) [[unlikely]] {
      auto const exception_ptr_copy = std::get<std::exception_ptr>(m_state);
      m_state = 0uz;  // destroy exception_ptr and set state to `usize`
      std::rethrow_exception(exception_ptr_copy);
    }
  }

private:
  friend class promise_base;

  template<typename>
  friend class future_promise_type;

  template<typename>
  friend class future;

  friend class runtime_base;

  template<usize N>
  friend class runtime;

  friend class context_lease;

  // Should stay within a cache-line of 64 bytes (8 words) on 64-bit systems
  std::coroutine_handle<> m_active_handle = std::noop_coroutine();  // word 1
  std::span<byte> m_stack{};                                        // word 3-4
  usize m_stack_index = 0;                                          // word 5
  std::variant<usize, blocked_by, std::exception_ptr> m_state;      // word 6-7
  transition_handler* m_transition_handler = nullptr;               // word 2
};

static_assert(sizeof(context) <= std::hardware_constructive_interference_size);
}  // namespace async::inline v0
