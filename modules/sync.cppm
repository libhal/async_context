
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

#include <coroutine>

export module async_context:sync;

export import :coroutine;

namespace async::inline v0 {
/**
 * @brief A RAII-style guard for exclusive access to a context
 *
 * The mutex class provides a mechanism for managing exclusive
 * access to a context, particularly in scenarios involving synchronization
 * primitives like mutexes or semaphores. It ensures proper cleanup and
 * unblocking when the guard goes out of scope.
 *
 * This is particularly useful for implementing resource management in
 * coroutine-based systems where proper cleanup and blocking state
 * transitions are required.
 */
export class mutex
{
public:
  /**
   * @brief Default constructor for mutex
   *
   * Creates an uninitialized mutex guard.
   */
  constexpr mutex() = default;

  /**
   * @brief Constructor that captures a context for exclusive access
   *
   * @param p_context The context to capture for exclusive access
   */
  constexpr mutex(context& p_context) noexcept
    : m_owner(&p_context)
  {
  }

  /**
   * @brief Assignment operator to capture a new context
   *
   * @param p_context The context to capture for exclusive access
   * @return Reference to this mutex instance
   */
  constexpr mutex& operator=(context& p_context) noexcept
  {
    m_owner = &p_context;
    return *this;
  }

  /**
   * @brief Assignment operator to clear the context capture
   *
   * @param p_context nullptr to clear the capture
   * @return Reference to this mutex instance
   */
  constexpr mutex& operator=(nullptr_t) noexcept
  {
    m_owner = nullptr;
    return *this;
  }

  /**
   * @brief Copy constructor for mutex
   *
   * @param p_context The mutex instance to copy from
   */
  constexpr mutex(mutex const& p_context) noexcept = default;

  /**
   * @brief Copy assignment operator for mutex
   *
   * @param p_context The mutex instance to copy from
   * @return Reference to this mutex instance
   */
  constexpr mutex& operator=(mutex const& p_context) noexcept = default;

  /**
   * @brief Move constructor for mutex
   *
   * @param p_context The mutex instance to move from
   */
  constexpr mutex(mutex&& p_context) noexcept = default;

  /**
   * @brief Move assignment operator for mutex
   *
   * @param p_context The mutex instance to move from
   * @return Reference to this mutex instance
   */
  constexpr mutex& operator=(mutex& p_context) noexcept = default;

  /**
   * @brief Equality operator to check if this guard holds a specific context
   *
   * @param p_context The context to compare against
   * @return true if this guard holds the specified context, false otherwise
   */
  constexpr bool operator==(context& p_context) noexcept
  {
    return m_owner == &p_context;
  }

  class guard
  {
  public:
    guard(mutex* p_access, context* p_context)
      : m_access(p_access)
      , m_context(p_context)
    {
    }

    ~guard()
    {
      release();
    }

    guard(guard const&) = delete;
    guard& operator=(guard const&) = delete;

    guard(guard&& p_other) noexcept
      : m_access(p_other.m_access)
      , m_context(p_other.m_context)
    {
      p_other.m_access = nullptr;
      p_other.m_context = nullptr;
    }

    guard& operator=(guard&& p_other) noexcept
    {
      if (this != &p_other) {
        release();
        m_access = p_other.m_access;
        m_context = p_other.m_context;
        p_other.m_access = nullptr;
        p_other.m_context = nullptr;
      }
      return *this;
    }

  private:
    mutex* m_access;
    context* m_context;

    void release()
    {
      if (m_access and m_context and m_context == m_access->m_owner) {
        m_access->m_owner = nullptr;
      }
    }
  };

  /**
   * @brief Acquire exclusive access, blocking until available
   *
   * Blocks the provided context until the resource is free, then claims it and
   * returns a guard that holds access until destroyed.
   *
   * @param p_ctx Context to block if the resource is in use
   * @return Guard that releases the resource on destruction
   */
  [[nodiscard]] future<guard> lock(context& p_ctx)
  {
    while (in_use()) {
      co_await p_ctx.block_by_sync(*m_owner);
    }
    m_owner = &p_ctx;
    co_return guard{ this, &p_ctx };
  }

  /**
   * @brief Get the address of the owning context
   *
   * @returns nullptr if the resource is not in use OR the address of the
   * context holding this resource.
   */
  [[nodiscard]] context const* owner() const
  {
    return m_owner;
  }

  /**
   * @brief Check if this guard is currently holding a context
   *
   * @return true if the guard has an active context, false otherwise
   */
  [[nodiscard]] constexpr bool in_use() const noexcept
  {
    return m_owner != nullptr;
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
   * @brief Unblocks the associated context and clears this guard
   *
   * This method unblocks the context that was captured by this guard and
   * clears the guard's reference to it.
   */
  constexpr void unblock_and_release() noexcept
  {
    if (in_use()) {
      m_owner->unblock();
      m_owner = nullptr;
    }
  }

private:
  /**
   * @brief The address of the context being held, or nullptr if not in use
   */
  context* m_owner = nullptr;
};
}  // namespace async::inline v0
