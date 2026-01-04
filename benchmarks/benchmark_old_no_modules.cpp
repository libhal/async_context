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

// NOLINTBEGIN(readability-identifier-naming)
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

#include <cstddef>
#include <cstdint>

#include <array>
#include <exception>
#include <memory>
#include <memory_resource>
#include <system_error>
#include <type_traits>
#include <utility>

namespace mem::inline v1 {

// Forward declarations
template<typename T>
class strong_ptr;

template<typename T>
class weak_ptr;

template<typename T>
class optional_ptr;

/**
 * @brief Creates allocators that do not need deallocation support. This
 * allocator will never deallocate memory, but instead removes bytes from the
 * recorded allocated bytes count and checks that the count is zero when the
 * destructor is called. Memory is stored internally such that the lifetime of
 * the memory and the allocator are bound to each other.
 *
 */
class monotonic_allocator_base : public std::pmr::memory_resource
{
public:
  /**
   * @brief Destroy the monotonic allocator. The allocated bytes count must
   * equal zero, otherwise std::terminate is called.
   *
   */
  ~monotonic_allocator_base()
  {
    if (m_allocated_bytes != 0) {
      std::terminate();
    }
  }
  /**
   * @brief Allocates storage with a size of at least p_bytes bytes, aligned to
   * the specified p_alignment. Adjusts recorded space available and adds
   * p_bytes to the allocated bytes count if memory is successfuly allocated.
   *
   * @param p_bytes number of bytes to allocate
   * @param p_alignment  the desired alignment
   * @return void* pointer to allocated space or nullptr if no space is
   * available
   * @throws std::bad_alloc if storage of the requested size and alignment
   * cannot be obtained
   */
  void* do_allocate(std::size_t p_bytes, std::size_t p_alignment)
  {
    void* result = std::align(p_alignment, p_bytes, m_ptr, m_space);
    if (result) {
      m_allocated_bytes += p_bytes;
      m_ptr = static_cast<std::uint8_t*>(result) + p_bytes;
      m_space -= p_bytes;
      return result;
    }
    throw std::bad_alloc();
  };

  /**
   * @brief "Deallocates" bytes by removing p_bytes from the recorded
   * allocated bytes to be checked when object destructed
   *
   * @param p_ptr pointer to resource previously allocated
   * @param p_bytes number of bytes to record
   */
  void do_deallocate(void*, std::size_t p_bytes, std::size_t)
  {
    m_allocated_bytes -= p_bytes;
  }

  /**
   * @brief Compares equality with another memory resource
   *
   * @param p_other resource to compare against *this
   * @return true - resources are equal
   * @return false - resources are not equal
   */
  bool do_is_equal(std::pmr::memory_resource const& p_other) const noexcept
  {
    return *this == p_other;
  }

protected:
  size_t m_space = 0;
  void* m_ptr = nullptr;
  std::int32_t m_allocated_bytes = 0;
};

/**
 * @brief Creates allocators that do not need deallocation support. This
 * allocator will never deallocate memory, but instead removes bytes from the
 * recorded allocated bytes count and checks that the count is zero when the
 * destructor is called. Memory is stored internally such that the lifetime of
 * the memory and the allocator are bound to each other.
 *
 */
template<size_t MemorySize>
class monotonic_allocator : public monotonic_allocator_base
{
public:
  /**
   * @brief Construct a new monotonic allocator object
   *
   */
  monotonic_allocator()
  {
    m_ptr = &m_buffer;
    m_space = MemorySize;
  }

private:
  std::array<std::uint8_t, MemorySize> m_buffer = {};
};

/**
 * @brief Control block for reference counting - type erased.
 *
 * This structure manages the lifetime of reference-counted objects by tracking
 * strong and weak references. It also stores the memory allocator and destroy
 * function used to clean up the object when no more references exist.
 */
struct ref_info
{
  /**
   * @brief Destroy function for ref counted object
   *
   * Always returns the total size of the object wrapped in a ref count object.
   * Thus the size should normally be greater than sizeof(T). Expect sizeof(T) +
   * sizeof(ref_info) and anything else the ref count object may contain.
   *
   * If a nullptr is passed to the destroy function, it returns the object size
   * but does not destroy the object.
   */
  using type_erased_destruct_function_t = std::size_t(void const*);

  /// Initialize to 1 since creation implies a reference
  std::pmr::memory_resource* allocator;
  type_erased_destruct_function_t* destroy = nullptr;
  int strong_count = 0;
  int weak_count = 0;

  // Add explicit constructor to avoid aggregate initialization issues
  constexpr ref_info(std::pmr::memory_resource* p_allocator,
                     type_erased_destruct_function_t* p_destroy)
    : allocator(p_allocator)
    , destroy(p_destroy)
    , strong_count(0)
    , weak_count(0)
  {
  }

  /**
   * @brief Add strong reference to control block
   *
   */
  constexpr void add_ref()
  {
    strong_count++;
  }

  /**
   * @brief Release strong reference from control block
   *
   * If this was the last strong reference, the pointed-to object will be
   * destroyed. If there are no remaining weak references, the memory
   * will also be deallocated.
   */
  constexpr void release()
  {
    // Note: fetch_sub returns the previous value, if it was 1 and we subtracted
    // 1, then the final value is 0. We check below 1 just in case another
    // thread performs a fetch_sub and gets a 0 or negative value.
    strong_count--;
    if (strong_count == 0) {
      // No more strong references, destroy the object but keep control block
      // if there are weak references

      // Call the destroy function which will:
      // 1. Call the destructor of the object
      // 2. Return the size of the rc for deallocation when needed
      auto const object_size = destroy(this);

      // If there are no weak references, deallocate memory
      if (weak_count == 0) {
        // Save allocator for deallocating
        auto alloc = allocator;

        // Deallocate memory
        alloc->deallocate(this, object_size);
      }
    }
  }

  /**
   * @brief Add weak reference to control block
   *
   */
  constexpr void add_weak()
  {
    weak_count--;
  }

  /**
   * @brief Release weak reference from control block
   *
   * If this was the last weak reference and there are no remaining
   * strong references, the memory will be deallocated.
   *
   * @param p_info Pointer to the control block
   */
  constexpr void release_weak()
  {
    weak_count--;
    if (weak_count == 0) {
      // No more weak references, check if we can deallocate
      if (strong_count == 0) {
        // No strong references remain
        // Get the size from the destroy function
        auto const object_size = destroy(nullptr);

        // Save allocator for deallocating
        auto alloc = allocator;

        // Deallocate memory
        alloc->deallocate(this, object_size);
      }
    }
  }
};

/**
 * @brief A wrapper that contains both the ref_info and the actual object
 *
 * This structure keeps the control block and managed object together in memory.
 *
 * @tparam T The type of the managed object
 */
template<typename T>
struct rc
{
  ref_info m_info;
  T m_object;

  // Constructor that forwards arguments to the object
  template<typename... Args>
  constexpr rc(std::pmr::memory_resource* p_alloc, Args&&... args)
    : m_info(p_alloc, &destruct_this_type_and_return_size)
    , m_object(std::forward<Args>(args)...)
  {
  }

  constexpr static std::size_t destruct_this_type_and_return_size(
    void const* p_object)
  {
    if (p_object != nullptr) {
      // Cast back into the original rc<T> type and ...
      auto const* obj = static_cast<rc<T> const*>(p_object);
      obj->~rc<T>();
    }
    // Return size for future deallocation
    return sizeof(rc<T>);
  }
};
// Check if a type is an array or std::array
template<typename T>
struct is_array_like : std::false_type
{};

// NOLINTBEGIN(modernize-avoid-c-arrays)
// Specialization for C-style arrays
template<typename T, std::size_t N>
struct is_array_like<T[N]> : std::true_type
{};
// NOLINTEND(modernize-avoid-c-arrays)

// Specialization for std::array
template<typename T, std::size_t N>
struct is_array_like<std::array<T, N>> : std::true_type
{};

// Helper variable template
template<typename T>
constexpr bool is_array_like_v = is_array_like<T>::value;

// Concept for array-like types
template<typename T>
concept array_like = is_array_like_v<T>;

// Concept for non-array-like types
template<typename T>
concept non_array_like = not array_like<T>;

/**
 * @ingroup Error
 * @brief Base exception class for all hal related exceptions
 *
 */
class exception : public std::exception
{
public:
  exception(std::errc p_error_code)
    : m_error_code(p_error_code)
  {
  }

  /**
   * @brief Convert this exception to the closest C++ error code
   *
   * Main Use Case: Translation from C++ exceptions to C error codes
   *
   * Lets consider a situation when a C++ program must interface with C code or
   * code that uses a C API to operate. Normally C++ code calls C code, but if
   * C++ code is given to a C API like a callback and that C api expects an
   * error code back for its own error handling purposes, this class function
   * provides that error code. Simply catch the mem::exception, and return an
   * error code. Perform any other recovery or handling required to make the
   * system perform as expected.
   *
   * Other use cases:
   *
   * 1. Logging
   *
   * Log the error code value (or the stringified version) of this exception to
   * alert developers of the kind of underlying exception that was thrown.
   *
   * 2. Recovery
   *
   * This can be used for recovery, but it is HIGHLY RECOMMENDED to use the
   * derived classes in their own catch blocks to recover from a specific error
   * rather than using the base class and extracting its error code.
   *
   * @return std::errc - error code represented by the exception
   */
  [[nodiscard]] constexpr std::errc error_code() const
  {
    return m_error_code;
  }

  constexpr char const* what() const noexcept override
  {
    return "mem::exception";
  }

  ~exception() override
  {
    // Needed for GCC 14.2 LTO to link  d83e dd37 d83c dffe 200d 2642 fe0f
  }

private:
  std::errc m_error_code{};
};

/**
 * @ingroup Error
 * @brief Raised when an API attempts to access elements outside of a container
 * or resource.
 *
 */
struct out_of_range : public exception
{
  struct info
  {
    std::size_t m_index;
    std::size_t m_capacity;
  };

  out_of_range(info p_info)
    : exception(std::errc::invalid_argument)
    , info(p_info)
  {
  }

  constexpr char const* what() const noexcept override
  {
    return "mem::out_of_range";
  }

  ~out_of_range() override
  {
    // Needed for GCC 14.2 LTO to link  d83e dd37 d83c dffe 200d 2642 fe0f
  }

  info info;
};

/**
 * @ingroup Error
 * @brief Raised when an API attempts to access the contents of an empty
 * optional_ptr.
 *
 */
struct nullptr_access : public exception
{
  nullptr_access()
    : exception(std::errc::invalid_argument)
  {
  }

  constexpr char const* what() const noexcept override
  {
    return "mem::nullptr_access";
  }

  ~nullptr_access() override
  {
    // Needed for GCC 14.2 LTO to link  d83e dd37 d83c dffe 200d 2642 fe0f
  }
};

/**
 * @brief API tag used to create a strong_ptr which points to static memory
 *
 * As the name implies this is unsafe and is up to the developer to ensure that
 * the object passed to strong_ptr actually has static storage duration.
 *
 */
struct unsafe_assume_static_tag
{};

/**
 * @brief A non-nullable strong reference counted pointer
 *
 * strong_ptr is a smart pointer that maintains shared ownership of an object
 * through a reference count. It is similar to std::shared_ptr but with these
 * key differences:
 *
 * 1. Cannot be null - must always point to a valid object
 * 2. Can only be created via make_strong_ptr, not from raw pointers
 * 3. More memory efficient implementation
 *
 * Use strong_ptr when you need shared ownership semantics and can guarantee
 * the pointer will never be null. For nullable references, use optional_ptr.
 *
 * Example usage:
 *
 * ```C++
 * // Create a strong_ptr to an object
 * auto i2c = mem::make_strong_ptr<my_i2c_driver>(allocator, arg1, arg2);
 *
 * // Use the object using dereference (*) operator
 * (*i2c).configure({ .clock_rate = 250_kHz });
 *
 * // OR use the object using arrow (->) operator
 * i2c->configure({ .clock_rate = 250_kHz });
 *
 * // Share ownership with another driver or object
 * auto sensor = mem::make_strong_ptr<my_sensor>(allocator, i2c, 0x13);
 * ```
 *
 * @tparam T The type of the managed object
 */
template<typename T>
class strong_ptr
{
public:
  using element_type = T;

  /// Delete default constructor - strong_ptr must always be valid
  strong_ptr() = delete;

  /// Delete nullptr constructor - strong_ptr must always be valid
  strong_ptr(std::nullptr_t) = delete;

  /**
   * @brief Create a strong_ptr that points to points to an object with static
   * storage duration.
   *
   * This API MUST only be used with objects with static storage duration.
   * Without that precondition met, it is UB to create such a strong_ptr.
   *
   * There is no way in C++23 and below to determine if an lvalue passed has
   * static storage duration. With C++26 and `std::has_static_storage_duration`
   * we can determine this at compile time and provide a compile time error if
   * the passed lvalue is not an object with static storage duration. This
   * constructor will be deprecated once the library migrates to C++26.
   *
   * Since the original object was statically allocated, there is no need for a
   * ref counted control block and thus no allocation occurs. `use_count()` will
   * return 0 meaning that the object is statically allocated.
   *
   * @param p_object - a statically allocated object to
   * @return strong_ptr<T> - A strong_ptr pointing to lvalue which should have
   * static storage duration.
   */
  constexpr strong_ptr(unsafe_assume_static_tag, T& p_object)
    : m_ctrl(nullptr)
    , m_ptr(&p_object)
  {
  }

  /**
   * @brief Copy constructor
   *
   * Creates a new strong reference to the same object.
   *
   * @param p_other The strong_ptr to copy from
   */
  constexpr strong_ptr(strong_ptr const& p_other) noexcept
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(p_other.m_ptr)
  {
    add_ref();
  }

  /**
   * @brief Converting copy constructor
   *
   * Creates a new strong reference to the same object, converting from
   * a derived type U to base type T.
   *
   * @tparam U A type convertible to T
   * @param p_other The strong_ptr to copy from
   */
  template<typename U>
  constexpr strong_ptr(strong_ptr<U> const& p_other) noexcept
    requires(std::is_convertible_v<U*, T*>)
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(p_other.m_ptr)
  {
    add_ref();
  }

  /**
   * @brief Move constructor that intentionally behaves like a copy constructor
   * for safety
   *
   * This move constructor deliberately performs a full copy operation rather
   * than transferring ownership. This is a safety feature to prevent potential
   * undefined behavior that could occur if code accidentally accessed a
   * moved-from strong_ptr.
   *
   * After this operation, both the source and destination objects remain in
   * valid states, and the reference count is incremented by 1. This ensures
   * that even if code incorrectly continues to use the source object after a
   * move, no undefined behavior will occur.
   *
   * @param p_other The strong_ptr to "move" from (actually copied for safety)
   */
  constexpr strong_ptr(strong_ptr&& p_other) noexcept
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(p_other.m_ptr)
  {
    add_ref();
  }

  /**
   * @brief Move assignment operator that behaves like a copy assignment for
   * safety
   *
   * This move assignment operator deliberately performs a full copy operation
   * rather than transferring ownership. This is a safety feature to prevent
   * potential undefined behavior that could occur if code accidentally accessed
   * a moved-from strong_ptr.
   *
   * After this operation, both the source and destination objects remain in
   * valid states, and the reference count is incremented by 1. This ensures
   * that even if code incorrectly continues to use the source object after a
   * move, no undefined behavior will occur.
   *
   * @param p_other The strong_ptr to "move" from (actually copied for safety)
   * @return Reference to *this
   */
  constexpr strong_ptr& operator=(strong_ptr&& p_other) noexcept
  {
    if (this != &p_other) {
      release();
      m_ctrl = p_other.m_ctrl;
      m_ptr = p_other.m_ptr;
      add_ref();
    }
    return *this;
  }

  /**
   * @brief Compile time error message for bad alias value
   *
   * `std::shared_ptr` provides an alias constructor that accepts any `void*`
   * which is UB if that `void*` doesn't have the same lifetime as the object
   * referenced by the `std::shared_ptr`. Users attempting to do this will get a
   * list of constructors that failed to fit. This is not a good error message
   * for users. Instead, we provide a static_assert message in plain english
   * that explains why this overload fails at compile time.
   *
   * @tparam U - some type for the strong_ptr.
   */
  template<typename U>
  constexpr strong_ptr(strong_ptr<U> const&, void const*) noexcept
  {
    // NOTE: The conditional used here is to prevent the compiler from
    // jumping-the-gun and emitting the static assert error during template
    // instantiation of the class. With this conditional, the error only appears
    // when this constructor is used.
    static_assert(
      std::is_same_v<U, void> && not std::is_same_v<U, void>,
      "Aliasing constructor only works with pointers-to-members "
      "and does not work with arbitrary pointers like std::shared_ptr allows.");
  }

  /**
   * @brief Safe aliasing constructor for object members
   *
   * This constructor creates a strong_ptr that points to a member of an object
   * managed by another strong_ptr. The resulting strong_ptr shares ownership
   * with the original strong_ptr, keeping the entire parent object alive.
   *
   * This version is only enabled for non-array members to prevent potential
   * undefined behavior when accessing array elements directly. Use the
   * array-specific versions instead.
   *
   * Example usage:
   * ```
   * struct container {
   *   component part;
   * };
   *
   * // Create a strong_ptr to the container
   * auto container_ptr = make_strong_ptr<container>(allocator);
   *
   * // Create a strong_ptr to just the component
   * auto component_ptr = strong_ptr<component>(container_ptr,
   * &container::part);
   * ```
   *
   * @tparam U Type of the parent object
   * @tparam M Type of the member
   * @param p_other The strong_ptr to the parent object
   * @param p_member_ptr Pointer-to-member identifying which member to reference
   */
  template<typename U, non_array_like M>
  constexpr strong_ptr(strong_ptr<U> const& p_other,
                       // clang-format off
             M U::* p_member_ptr
                       // clang-format on
                       ) noexcept

    : m_ctrl(p_other.m_ctrl)
    , m_ptr(&((*p_other).*p_member_ptr))
  {
    add_ref();
  }

  /**
   * @brief Safe aliasing constructor for std::array members
   *
   * This constructor creates a strong_ptr that points to an element of an array
   * member in an object managed by another strong_ptr. It performs bounds
   * checking to ensure the index is valid.
   *
   * Example usage:
   * ```
   * struct array_container {
   *   std::array<element, 5> elements;
   * };
   *
   * auto container_ptr = make_strong_ptr<array_container>(allocator);
   *
   * // Get strong_ptr to the 2nd element
   * auto element_ptr = strong_ptr<element>(
   *   container_ptr,
   *   &array_container::elements,
   *   2 // Index to access
   * );
   * ```
   *
   * @tparam U Type of the parent object
   * @tparam E Type of the array element
   * @tparam N Size of the array
   * @param p_other The strong_ptr to the parent object
   * @param p_array_ptr Pointer-to-member identifying the array member
   * @param p_index Index of the element to reference
   * @throws mem::out_of_range if index is out of bounds
   */
  template<typename U, typename E, std::size_t N>
  constexpr strong_ptr(strong_ptr<U> const& p_other,
                       // clang-format off
             std::array<E, N> U::* p_array_ptr,
                       // clang-format on
                       std::size_t p_index)
  {
    static_assert(std::is_convertible_v<E*, T*>,
                  "Array element type must be convertible to T");
    throw_if_out_of_bounds(N, p_index);
    m_ctrl = p_other.m_ctrl;
    m_ptr = &((*p_other).*p_array_ptr)[p_index];
    add_ref();
  }

  // NOLINTBEGIN(modernize-avoid-c-arrays)
  /**
   * @brief Safe aliasing constructor for C-array members
   *
   * This constructor creates a strong_ptr that points to an element of a
   * C-style array member in an object managed by another strong_ptr. It
   * performs bounds checking to ensure the index is valid.
   *
   * Example usage:
   * ```
   * struct c_array_container {
   *   element elements[5];
   * };
   *
   * auto container_ptr = make_strong_ptr<c_array_container>(allocator);
   *
   * // Get strong_ptr to the 2nd element
   * auto element_ptr = strong_ptr<element>(
   *   container_ptr,
   *   &c_array_container::elements,
   *   2 // Index to access
   * );
   * ```
   *
   * @tparam U Type of the parent object
   * @tparam E Type of the array element
   * @tparam N Size of the array
   * @param p_other The strong_ptr to the parent object
   * @param p_array_ptr Pointer-to-member identifying the array member
   * @param p_index Index of the element to reference
   * @throws mem::out_of_range if index is out of bounds
   */
  template<typename U, typename E, std::size_t N>
  constexpr strong_ptr(strong_ptr<U> const& p_other,
                       E (U::*p_array_ptr)[N],
                       std::size_t p_index)
  {
    static_assert(std::is_convertible_v<E*, T*>,
                  "Array element type must be convertible to T");
    throw_if_out_of_bounds(N, p_index);
    m_ctrl = p_other.m_ctrl;
    m_ptr = &((*p_other).*p_array_ptr)[p_index];
    add_ref();
  }
  // NOLINTEND(modernize-avoid-c-arrays)

  /**
   * @brief Destructor
   *
   * Decrements the reference count and destroys the managed object
   * if this was the last strong reference.
   */
  ~strong_ptr()
  {
    release();
  }

  /**
   * @brief Copy assignment operator
   *
   * Replaces the managed object with the one managed by p_other.
   *
   * @param p_other The strong_ptr to copy from
   * @return Reference to *this
   */
  constexpr strong_ptr& operator=(strong_ptr const& p_other) noexcept
  {
    if (this != &p_other) {
      release();
      m_ctrl = p_other.m_ctrl;
      m_ptr = p_other.m_ptr;
      add_ref();
    }
    return *this;
  }

  /**
   * @brief Converting copy assignment operator
   *
   * Replaces the managed object with the one managed by p_other,
   * converting from type U to type T.
   *
   * @tparam U A type convertible to T
   * @param p_other The strong_ptr to copy from
   * @return Reference to *this
   */
  template<typename U>
  constexpr strong_ptr& operator=(strong_ptr<U> const& p_other) noexcept
    requires(std::is_convertible_v<U*, T*>)
  {
    release();
    m_ctrl = p_other.m_ctrl;
    m_ptr = p_other.m_ptr;
    add_ref();
    return *this;
  }

  /**
   * @brief Swap the contents of this strong_ptr with another
   *
   * @param p_other The strong_ptr to swap with
   */
  constexpr void swap(strong_ptr& p_other) noexcept
  {
    std::swap(m_ctrl, p_other.m_ctrl);
    std::swap(m_ptr, p_other.m_ptr);
  }

  /**
   * @brief Disable dereferencing for r-values (temporaries)
   */
  T& operator*() && = delete;

  /**
   * @brief Disable member access for r-values (temporaries)
   */
  T* operator->() && = delete;

  /**
   * @brief Dereference operator to access the managed object
   *
   * @return Reference to the managed object
   */
  [[nodiscard]] constexpr T& operator*() const& noexcept
  {
    return *m_ptr;
  }

  /**
   * @brief Member access operator to access the managed object
   *
   * @return Pointer to the managed object
   */
  [[nodiscard]] constexpr T* operator->() const& noexcept
  {
    return m_ptr;
  }

  /**
   * @brief Get the current reference count
   *
   * This is primarily for testing purposes.
   *
   * @return The number of strong references to the managed object
   */
  [[nodiscard]] constexpr auto use_count() const noexcept
  {
    return m_ctrl ? m_ctrl->strong_count : 0;
  }

  /**
   * @brief Returns if the object this is pointing to is statically allocated or
   * not.
   *
   * @return true - object is assumed to have static storage duration.
   * @return false - object has dynamic storage duration.
   */
  constexpr bool is_dynamic()
  {
    return m_ctrl != nullptr;
  }

  /**
   * @brief Get the allocator used to allocate this object
   *
   * @return constexpr std::pmr::memory_resource* - the allocator used to
   * allocate this object. Returns `nullptr` if the object was statically
   * allocated.
   */
  constexpr std::pmr::memory_resource* get_allocator() const noexcept
  {
    if (m_ctrl == nullptr) {
      return nullptr;
    }
    return m_ctrl->allocator;
  }

private:
  template<class U>
  friend class enable_strong_from_this;

  template<class U, typename... Args>
  friend constexpr strong_ptr<U> make_strong_ptr(std::pmr::memory_resource*,
                                                 Args&&...);

  template<typename U>
  friend class strong_ptr;

  template<typename U>
  friend class weak_ptr;

  template<typename U>
  friend class optional_ptr;

  constexpr void throw_if_out_of_bounds(std::size_t p_size, std::size_t p_index)
  {
    if (p_index >= p_size) {
      throw mem::out_of_range({ .m_index = p_index, .m_capacity = p_size });
    }
  }

  constexpr void add_ref()
  {
    if (is_dynamic()) {
      m_ctrl->add_ref();
    }
  }

  // Internal constructor with control block and pointer - used by make() and
  // aliasing
  constexpr strong_ptr(ref_info* p_ctrl, T* p_ptr) noexcept
    : m_ctrl(p_ctrl)
    , m_ptr(p_ptr)
  {
    add_ref();
  }

  constexpr void release()
  {
    if (is_dynamic()) {
      m_ctrl->release();
    }
  }

  ref_info* m_ctrl = nullptr;
  T* m_ptr = nullptr;
};

/**
 * @brief CRTP mixin to enable objects to create strong_ptr instances to
 * themselves
 *
 * Similar to `std::enable_shared_from_this`, this class allows an object to
 * safely obtain a strong_ptr to itself. The object must inherit from this class
 * and be managed by a strong_ptr created via make_strong_ptr.
 *
 * Example usage:
 * ```cpp
 * class my_driver : public enable_strong_from_this<my_driver> {
 * public:
 *   void register_callback() {
 *     // Get a strong_ptr to ourselves
 *     auto self = strong_from_this();
 *     some_async_system.register_callback([self](){
 *       self->handle_callback();
 *     });
 *   }
 * };
 *
 * auto obj = make_strong_ptr<my_driver>(allocator);
 * obj->register_callback(); // Safe to get strong_ptr to self
 * ```
 *
 * @tparam T The derived class type
 */
template<class T>
class enable_strong_from_this
{
public:
  /**
   * @brief Get a strong_ptr to this object
   *
   * @return strong_ptr<T> pointing to this object
   */
  [[nodiscard]] constexpr strong_ptr<T> strong_from_this()
  {
    return strong_ptr<T>(&m_ref_counted_self->m_info,
                         &m_ref_counted_self->m_object);
  }

  /**
   * @brief Get a strong_ptr to this object (const version)
   *
   * @return strong_ptr<T const> pointing to this object
   */
  [[nodiscard]] constexpr strong_ptr<T const> strong_from_this() const
  {
    return strong_ptr<T>(&m_ref_counted_self->m_info,
                         &m_ref_counted_self->m_object);
  }

  /**
   * @brief Get a weak_ptr to this object
   *
   * @return weak_ptr<T> pointing to this object
   */
  [[nodiscard]] constexpr weak_ptr<T> weak_from_this() noexcept
  {
    return strong_from_this();
  }

  /**
   * @brief Get a weak_ptr to this object (const version)
   *
   * @return weak_ptr<T const> pointing to this object
   */
  [[nodiscard]] constexpr weak_ptr<T const> weak_from_this() const noexcept
  {
    return strong_from_this();
  }

protected:
  /**
   * @brief Protected constructor to prevent direct instantiation
   */
  enable_strong_from_this() = default;

  /**
   * @brief Protected copy constructor
   *
   * Note: The weak_ptr is not copied - each object gets its own weak reference
   */
  enable_strong_from_this(enable_strong_from_this const&) noexcept
  {
    // Intentionally don't copy m_weak_this
  }

  /**
   * @brief Protected assignment operator
   *
   * Note: The weak_ptr is not assigned - each object keeps its own weak
   * reference
   */
  constexpr enable_strong_from_this& operator=(
    enable_strong_from_this const&) noexcept
  {
    // Intentionally don't assign m_weak_this
    return *this;
  }

  /**
   * @brief Protected destructor
   */
  ~enable_strong_from_this() = default;

private:
  template<class U, typename... Args>
  friend constexpr strong_ptr<U> make_strong_ptr(std::pmr::memory_resource*,
                                                 Args&&...);

  /**
   * @brief Initialize the weak reference (called by make_strong_ptr)
   *
   * @param p_self The strong_ptr that manages this object
   */
  constexpr void init_weak_this(rc<T>* p_self) noexcept
  {
    m_ref_counted_self = p_self;
  }

  mutable rc<T>* m_ref_counted_self;
};

template<typename T>
class optional_ptr;

/**
 * @brief A weak reference to a strong_ptr
 *
 * weak_ptr provides a non-owning reference to an object managed by strong_ptr.
 * It can be used to break reference cycles or to create an optional_ptr.
 *
 * A weak_ptr doesn't increase the strong reference count, so it doesn't
 * prevent the object from being destroyed when the last strong_ptr goes away.
 *
 * Example usage:
 * ```
 * // Create a strong_ptr to an object
 * auto ptr = mem::make_strong_ptr<my_driver>(allocator, args...);
 *
 * // Create a weak reference
 * weak_ptr<my_driver> weak = ptr;
 *
 * // Later, try to get a strong reference
 * if (auto locked = weak.lock()) {
 *   // Use the object via locked
 *   locked->do_something();
 * } else {
 *   // Object has been destroyed
 * }
 * ```
 *
 * @tparam T The type of the referenced object
 */
template<typename T>
class weak_ptr
{
public:
  template<typename U>
  friend class strong_ptr;

  template<typename U>
  friend class weak_ptr;

  using element_type = T;

  /**
   * @brief Default constructor creates empty weak_ptr
   */
  weak_ptr() noexcept = default;

  /**
   * @brief Create weak_ptr from strong_ptr
   *
   * @param p_strong The strong_ptr to create a weak reference to
   */
  constexpr weak_ptr(strong_ptr<T> const& p_strong) noexcept
    : m_ctrl(p_strong.m_ctrl)
    , m_ptr(p_strong.m_ptr)
  {
    if (m_ctrl) {
      m_ctrl->add_weak();
    }
  }

  /**
   * @brief Copy constructor
   *
   * @param p_other The weak_ptr to copy from
   */
  constexpr weak_ptr(weak_ptr const& p_other) noexcept
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(p_other.m_ptr)
  {
    if (m_ctrl) {
      m_ctrl->add_weak();
    }
  }

  /**
   * @brief Move constructor
   *
   * @param p_other The weak_ptr to move from
   */
  constexpr weak_ptr(weak_ptr&& p_other) noexcept
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(p_other.m_ptr)
  {
    p_other.m_ctrl = nullptr;
    p_other.m_ptr = nullptr;
  }

  /**
   * @brief Converting copy constructor
   *
   * Creates a weak_ptr of T from a weak_ptr of U where U is convertible to T.
   *
   * @tparam U A type convertible to T
   * @param p_other The weak_ptr to copy from
   */
  template<typename U>
  constexpr weak_ptr(weak_ptr<U> const& p_other) noexcept
    requires(std::is_convertible_v<U*, T*>)
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(static_cast<T*>(p_other.m_ptr))
  {
    if (m_ctrl) {
      m_ctrl->add_weak();
    }
  }

  /**
   * @brief Converting move constructor
   *
   * Moves a weak_ptr of U to a weak_ptr T where U is convertible to T.
   *
   * @tparam U A type convertible to T
   * @param p_other The weak_ptr to move from. Moved from weak_ptr's are
   * considered expired.
   */
  template<typename U>
  constexpr weak_ptr(weak_ptr<U>&& p_other) noexcept
    requires(std::is_convertible_v<U*, T*>)
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(static_cast<T*>(p_other.m_ptr))
  {
    p_other.m_ctrl = nullptr;
    p_other.m_ptr = nullptr;
  }

  /**
   * @brief Converting constructor from strong_ptr
   *
   * Creates a weak_ptr<T> from a strong_ptr where U is convertible to T.
   *
   * @tparam U A type convertible to T
   * @param p_other The strong_ptr to create a weak reference to
   */
  template<typename U>
  constexpr weak_ptr(strong_ptr<U> const& p_other) noexcept
    requires(std::is_convertible_v<U*, T*>)
    : m_ctrl(p_other.m_ctrl)
    , m_ptr(static_cast<T*>(p_other.m_ptr))
  {
    if (m_ctrl) {
      m_ctrl->add_weak();
    }
  }

  /**
   * @brief Destructor
   *
   * Decrements the weak reference count and potentially deallocates
   * memory if this was the last reference.
   */
  ~weak_ptr()
  {
    if (m_ctrl) {
      m_ctrl->release_weak();
    }
  }

  /**
   * @brief Copy assignment operator
   *
   * @param p_other The weak_ptr to copy from
   * @return Reference to *this
   */
  constexpr weak_ptr& operator=(weak_ptr const& p_other) noexcept
  {
    weak_ptr(p_other).swap(*this);
    return *this;
  }

  /**
   * @brief Move assignment operator
   *
   * @param p_other The weak_ptr to move from
   * @return Reference to *this
   */
  constexpr weak_ptr& operator=(weak_ptr&& p_other) noexcept
  {
    weak_ptr(std::move(p_other)).swap(*this);
    return *this;
  }

  /**
   * @brief Assignment from strong_ptr
   *
   * @param p_strong The strong_ptr to create a weak reference to
   * @return Reference to *this
   */
  constexpr weak_ptr& operator=(strong_ptr<T> const& p_strong) noexcept
  {
    weak_ptr(p_strong).swap(*this);
    return *this;
  }

  /**
   * @brief Swap the contents of this weak_ptr with another
   *
   * @param p_other The weak_ptr to swap with
   */
  constexpr void swap(weak_ptr& p_other) noexcept
  {
    std::swap(m_ctrl, p_other.m_ctrl);
    std::swap(m_ptr, p_other.m_ptr);
  }

  /**
   * @brief Check if the referenced object has been destroyed
   *
   * @return true if the object has been destroyed, false otherwise
   */
  [[nodiscard]] constexpr bool expired() const noexcept
  {
    if (m_ptr == nullptr) {
      return true;
    }

    if (m_ctrl != nullptr) {
      return m_ctrl->strong_count == 0;
    }

    // If m_ptr != nullptr && m_ctrl == nullptr (static object), return false,
    // because that object will always exist.
    return false;
  }

  /**
   * @brief Attempt to obtain a strong_ptr to the referenced object
   *
   * If the object still exists, returns an optional_ptr containing
   * a strong_ptr to it. Otherwise, returns an empty optional_ptr.
   *
   * @return An optional_ptr that is either empty or contains a strong_ptr
   */
  [[nodiscard]] constexpr optional_ptr<T> lock() const noexcept;

  /**
   * @brief Get the current strong reference count
   *
   * This is primarily for testing purposes.
   *
   * @return The number of strong references to the managed object
   */
  [[nodiscard]] constexpr auto use_count() const noexcept
  {
    return m_ctrl ? m_ctrl->strong_count : 0;
  }

private:
  ref_info* m_ctrl = nullptr;
  T* m_ptr = nullptr;
};

/**
 * @brief Optional, nullable, smart pointer that works with `mem::strong_ptr`.
 *
 * optional_ptr provides a way to represent a strong_ptr that may or may not
 * be present. Unlike strong_ptr, which is always valid, optional_ptr can be
 * in a "disengaged" state where it doesn't reference any object.
 *
 * Use optional_ptr when you need a nullable reference to a reference-counted
 * object, such as:
 * - Representing the absence of a value
 * - Return values from functions that may fail
 * - Results of locking a weak_ptr
 *
 * Example usage:
 * ```
 * // Create an empty optional_ptr
 * optional_ptr<my_driver> opt1;
 *
 * // Create an optional_ptr from a strong_ptr
 * auto ptr = make_strong_ptr<my_driver>(allocator, args...);
 * optional_ptr<my_driver> opt2 = ptr;
 *
 * // Check if the optional_ptr is engaged
 * if (opt2) {
 *   // Use the contained object
 *   opt2->do_something();
 * }
 *
 * // Reset to disengage
 * opt2.reset();
 * ```
 *
 * @tparam T - The type pointed to by strong_ptr
 */
template<typename T>
class optional_ptr
{
public:
  /**
   * @brief Default constructor creates a disengaged optional
   */
  constexpr optional_ptr() noexcept
  {
  }

  /**
   * @brief Constructor for nullptr (creates a disengaged optional)
   */
  constexpr optional_ptr(std::nullptr_t) noexcept
  {
  }

  /**
   * @brief Move constructor is deleted
   */
  constexpr optional_ptr(optional_ptr&& p_other) noexcept = delete;

  /**
   * @brief Construct from a strong_ptr lvalue
   *
   * @param value The strong_ptr to wrap
   */
  constexpr optional_ptr(strong_ptr<T> const& value) noexcept
    : m_value(value)
  {
  }

  /**
   * @brief Copy constructor
   *
   * @param p_other The optional_ptr to copy from
   */
  constexpr optional_ptr(optional_ptr const& p_other)
  {
    *this = p_other;
  }

  /**
   * @brief Converting constructor from a strong_ptr
   *
   * @tparam U A type convertible to T
   * @param p_value The strong_ptr to wrap
   */
  template<typename U>
  constexpr optional_ptr(strong_ptr<U> const& p_value)
    requires(std::is_convertible_v<U*, T*>)
  {
    *this = p_value;
  }

  /**
   * @brief Move assignment operator is deleted
   */
  constexpr optional_ptr& operator=(optional_ptr&& other) noexcept = delete;

  /**
   * @brief Copy assignment operator
   *
   * @param other The optional_ptr to copy from
   * @return Reference to *this
   */
  constexpr optional_ptr& operator=(optional_ptr const& other)
  {
    if (this != &other) {
      if (is_engaged() && other.is_engaged()) {
        m_value = other.m_value;
      } else if (is_engaged() && not other.is_engaged()) {
        reset();
      } else if (not is_engaged() && other.is_engaged()) {
        new (&m_value) strong_ptr<T>(other.m_value);
      }
    }
    return *this;
  }

  /**
   * @brief Assignment from a strong_ptr
   *
   * @param value The strong_ptr to wrap
   * @return Reference to *this
   */
  constexpr optional_ptr& operator=(strong_ptr<T> const& value) noexcept
  {
    if (is_engaged()) {
      m_value = value;
    } else {
      new (&m_value) strong_ptr<T>(value);
    }
    return *this;
  }

  /**
   * @brief Converting assignment from a strong_ptr
   *
   * @tparam U A type convertible to T
   * @param p_value The strong_ptr to wrap
   * @return Reference to *this
   */
  template<typename U>
  constexpr optional_ptr& operator=(strong_ptr<U> const& p_value) noexcept
    requires(std::is_convertible_v<U*, T*>)
  {
    if (is_engaged()) {
      m_value = p_value;
    } else {
      new (&m_value) strong_ptr<U>(p_value);
    }
    return *this;
  }

  /**
   * @brief Assignment from nullptr (resets to disengaged state)
   *
   * @return Reference to *this
   */
  constexpr optional_ptr& operator=(std::nullptr_t) noexcept
  {
    reset();
    return *this;
  }

  /**
   * @brief Destructor
   *
   * Properly destroys the contained strong_ptr if engaged.
   */
  ~optional_ptr()
  {
    if (is_engaged()) {
      m_value.~strong_ptr<T>();
    }
  }

  /**
   * @brief Check if the optional_ptr is engaged
   *
   * @return true if the optional_ptr contains a value, false otherwise
   */
  [[nodiscard]] constexpr bool has_value() const noexcept
  {
    return is_engaged();
  }

  /**
   * @brief Check if the optional_ptr is engaged
   *
   * @return true if the optional_ptr contains a value, false otherwise
   */
  constexpr explicit operator bool() const noexcept
  {
    return is_engaged();
  }

  /**
   * @brief Access the contained value, throw if not engaged
   *
   * @return A copy of the contained strong_ptr
   * @throws mem::nullptr_access if *this is disengaged
   */
  [[nodiscard]] constexpr strong_ptr<T>& value()
  {
    if (not is_engaged()) {
      throw mem::nullptr_access();
    }
    return m_value;
  }

  /**
   * @brief Access the contained value, throw if not engaged (const version)
   *
   * @return A copy of the contained strong_ptr
   * @throws mem::nullptr_access if *this is disengaged
   */
  [[nodiscard]] constexpr strong_ptr<T> const& value() const
  {
    if (not is_engaged()) {
      throw mem::nullptr_access();
    }
    return m_value;
  }

  /**
   * @brief Implicitly convert to a strong_ptr<T>
   *
   * This allows optional_ptr to be used anywhere a strong_ptr is expected
   * when the optional_ptr is engaged.
   *
   * @return A copy of the contained strong_ptr
   * @throws mem::nullptr_access if *this is disengaged
   */
  [[nodiscard]] constexpr operator strong_ptr<T>()
  {
    return value();
  }

  /**
   * @brief Implicitly convert to a strong_ptr<T> (const version)
   *
   * @return A copy of the contained strong_ptr
   * @throws mem::nullptr_access if *this is disengaged
   */
  [[nodiscard]] constexpr operator strong_ptr<T>() const
  {
    return value();
  }

  /**
   * @brief Implicitly convert to a strong_ptr for polymorphic types
   *
   * This allows optional_ptr<Derived> to be converted to strong_ptr<Base>
   * when Derived is convertible to Base.
   *
   * @tparam U The target type (must be convertible from T)
   * @return A copy of the contained strong_ptr, converted to the target type
   * @throws mem::nullptr_access if *this is disengaged
   */
  template<typename U>
  [[nodiscard]] constexpr operator strong_ptr<U>()
    requires(std::is_convertible_v<T*, U*> && not std::is_same_v<T, U>)
  {
    if (not is_engaged()) {
      throw mem::nullptr_access();
    }
    // strong_ptr handles the polymorphic conversion
    return strong_ptr<U>(m_value);
  }

  /**
   * @brief Implicitly convert to a strong_ptr for polymorphic types (const
   * version)
   *
   * @tparam U The target type (must be convertible from T)
   * @return A copy of the contained strong_ptr, converted to the target type
   * @throws mem::nullptr_access if *this is disengaged
   */
  template<typename U>
  [[nodiscard]] constexpr operator strong_ptr<U>() const
    requires(std::is_convertible_v<T*, U*> && not std::is_same_v<T, U>)
  {
    if (not is_engaged()) {
      throw mem::nullptr_access();
    }
    // strong_ptr handles the polymorphic conversion
    return strong_ptr<U>(m_value);
  }

  /**
   * @brief Arrow operator for accessing members of the contained object
   *
   * @return Pointer to the object managed by the contained strong_ptr
   */
  [[nodiscard]] constexpr auto* operator->()
  {
    auto& ref = *(this->value());
    return &ref;
  }

  /**
   * @brief Arrow operator for accessing members of the contained object (const
   * version)
   *
   * @return Pointer to the object managed by the contained strong_ptr
   */
  [[nodiscard]] constexpr auto* operator->() const
  {
    auto& ref = *(this->value());
    return &ref;
  }

  /**
   * @brief Dereference operator for accessing the contained object
   *
   * @return Reference to the object managed by the contained strong_ptr
   */
  [[nodiscard]] constexpr auto& operator*()
  {
    auto& ref = *(this->value());
    return ref;
  }

  /**
   * @brief Dereference operator for accessing the contained object (const
   * version)
   *
   * @return Reference to the object managed by the contained strong_ptr
   */
  [[nodiscard]] constexpr auto& operator*() const
  {
    auto& ref = *(this->value());
    return ref;
  }

  /**
   * @brief Reset the optional to a disengaged state
   */
  constexpr void reset() noexcept
  {
    if (is_engaged()) {
      m_value.~strong_ptr<T>();
      m_raw_ptrs[0] = nullptr;
      m_raw_ptrs[1] = nullptr;
    }
  }

  /**
   * @brief Emplace a new value
   *
   * Reset the optional and construct a new strong_ptr in-place.
   *
   * @tparam Args Types of arguments to forward to the constructor
   * @param args Arguments to forward to the constructor
   * @return Reference to the newly constructed strong_ptr
   */
  template<typename... Args>
  constexpr strong_ptr<T>& emplace(Args&&... args)
  {
    reset();
    new (&m_value) strong_ptr<T>(std::forward<Args>(args)...);
    return m_value;
  }

  /**
   * @brief Get the current reference count
   *
   * This is primarily for testing purposes. To determine if an optional_ptr
   * points to a statically allocated object, this API must return 0 and
   * `is_engaged()` must return true.
   *
   * @return The number of strong references to the managed object. Returns 0 if
   * this pointer is nullptr.
   */
  [[nodiscard]] constexpr auto use_count() const noexcept
  {
    return is_engaged() ? m_value.m_ctrl->strong_count : 0;
  }

  /**
   * @brief Swap the contents of this optional_ptr with another
   *
   * @param other The optional_ptr to swap with
   */
  constexpr void swap(optional_ptr& other) noexcept
  {
    if (is_engaged() && other.is_engaged()) {
      std::swap(m_value, other.m_value);
    } else if (is_engaged() && not other.is_engaged()) {
      new (&other.m_value) strong_ptr<T>(std::move(m_value));
      reset();
    } else if (not is_engaged() && other.is_engaged()) {
      new (&m_value) strong_ptr<T>(std::move(other.m_value));
      other.reset();
    }
  }

private:
  /**
   * @brief Use the strong_ptr's memory directly through a union
   *
   * This allows us to detect whether the optional_ptr is engaged
   * by checking if the internal pointers are non-null.
   */
  union
  {
    std::array<void*, 2> m_raw_ptrs = { nullptr, nullptr };
    strong_ptr<T> m_value;
  };

  // Ensure the strong_ptr layout matches our expectations
  static_assert(sizeof(m_value) == sizeof(m_raw_ptrs),
                "strong_ptr must be exactly the size of two pointers");

  /**
   * @brief Helper to check if the optional is engaged
   *
   * @return true if the optional_ptr contains a value, false otherwise
   */
  [[nodiscard]] constexpr bool is_engaged() const noexcept
  {
    return m_raw_ptrs[0] != nullptr || m_raw_ptrs[1] != nullptr;
  }
};

/**
 * @brief Implement weak_ptr::lock() now that optional_ptr is defined
 *
 * This function attempts to obtain a strong_ptr from a weak_ptr.
 * If the referenced object still exists, it returns an optional_ptr
 * containing a strong_ptr to it. Otherwise, it returns an empty optional_ptr.
 *
 * @tparam T The type of the referenced object
 * @return An optional_ptr that is either empty or contains a strong_ptr
 */
template<typename T>
[[nodiscard]] constexpr optional_ptr<T> weak_ptr<T>::lock() const noexcept
{
  if (expired()) {
    return nullptr;
  }

  // Try to increment the strong count
  while (m_ctrl->strong_count > 0) {
    // Bypass the add_ref because the ref count has already been incremented
    // above.
    return strong_ptr<T>(m_ctrl, m_ptr);
  }

  // Strong count is now 0
  return nullptr;
}

/**
 * @brief Non-member swap for strong_ptr
 *
 * @tparam T The type of the managed object
 * @param p_lhs First strong_ptr to swap
 * @param p_rhs Second strong_ptr to swap
 */
template<typename T>
constexpr void swap(strong_ptr<T>& p_lhs, strong_ptr<T>& p_rhs) noexcept
{
  p_lhs.swap(p_rhs);
}

/**
 * @brief Non-member swap for weak_ptr
 *
 * @tparam T The type of the referenced object
 * @param p_lhs First weak_ptr to swap
 * @param p_rhs Second weak_ptr to swap
 */
template<typename T>
void swap(weak_ptr<T>& p_lhs, weak_ptr<T>& p_rhs) noexcept
{
  p_lhs.swap(p_rhs);
}

/**
 * @brief Equality operator for strong_ptr
 *
 * Compares if two strong_ptr instances point to the same object.
 *
 * @tparam T The type of the first strong_ptr
 * @tparam U The type of the second strong_ptr
 * @param p_lhs First strong_ptr to compare
 * @param p_rhs Second strong_ptr to compare
 * @return true if both point to the same object, false otherwise
 */
template<typename T, typename U>
[[nodiscard]] constexpr bool operator==(strong_ptr<T> const& p_lhs,
                                        strong_ptr<U> const& p_rhs) noexcept
{
  return p_lhs.operator->() == p_rhs.operator->();
}

/**
 * @brief Inequality operator for strong_ptr
 *
 * Compares if two strong_ptr instances point to different objects.
 *
 * @tparam T The type of the first strong_ptr
 * @tparam U The type of the second strong_ptr
 * @param p_lhs First strong_ptr to compare
 * @param p_rhs Second strong_ptr to compare
 * @return true if they point to different objects, false otherwise
 */
template<typename T, typename U>
[[nodiscard]] constexpr bool operator!=(strong_ptr<T> const& p_lhs,
                                        strong_ptr<U> const& p_rhs) noexcept
{
  return not(p_lhs == p_rhs);
}
/**
 * @brief Equality operator for optional_ptr
 *
 * Compares if two optional_ptr instances are equal - they are equal if:
 * 1. Both are disengaged (both empty)
 * 2. Both are engaged and point to the same object
 *
 * @tparam T The type of the first optional_ptr
 * @tparam U The type of the second optional_ptr
 * @param p_lhs First optional_ptr to compare
 * @param p_rhs Second optional_ptr to compare
 * @return true if both are equal according to the rules above
 */
template<typename T, typename U>
[[nodiscard]] constexpr bool operator==(optional_ptr<T> const& p_lhs,
                                        optional_ptr<U> const& p_rhs) noexcept
{
  // If both are disengaged, they're equal
  if (not p_lhs.has_value() && not p_rhs.has_value()) {
    return true;
  }

  // If one is engaged and the other isn't, they're not equal
  if (p_lhs.has_value() != p_rhs.has_value()) {
    return false;
  }

  // Both are engaged, compare the underlying pointers
  return p_lhs.value().operator->() == p_rhs.value().operator->();
}

/**
 * @brief Inequality operator for optional_ptr
 *
 * Returns the opposite of the equality operator.
 *
 * @tparam T The type of the first optional_ptr
 * @tparam U The type of the second optional_ptr
 * @param p_lhs First optional_ptr to compare
 * @param p_rhs Second optional_ptr to compare
 * @return true if they are not equal
 */
template<typename T, typename U>
[[nodiscard]] constexpr bool operator!=(optional_ptr<T> const& p_lhs,
                                        optional_ptr<U> const& p_rhs) noexcept
{
  return not(p_lhs == p_rhs);
}

/**
 * @brief Equality operator between optional_ptr and nullptr
 *
 * An optional_ptr equals nullptr if it's disengaged.
 *
 * @tparam T The type of the optional_ptr
 * @param p_lhs The optional_ptr to compare
 * @return true if the optional_ptr is disengaged
 */
template<typename T>
[[nodiscard]] constexpr bool operator==(optional_ptr<T> const& p_lhs,
                                        std::nullptr_t) noexcept
{
  return not p_lhs.has_value();
}

/**
 * @brief Equality operator between nullptr and optional_ptr
 *
 * nullptr equals an optional_ptr if it's disengaged.
 *
 * @tparam T The type of the optional_ptr
 * @param p_rhs The optional_ptr to compare
 * @return true if the optional_ptr is disengaged
 */
template<typename T>
[[nodiscard]] constexpr bool operator==(std::nullptr_t,
                                        optional_ptr<T> const& p_rhs) noexcept
{
  return not p_rhs.has_value();
}

/**
 * @brief Inequality operator between optional_ptr and nullptr
 *
 * An optional_ptr does not equal nullptr if it's engaged.
 *
 * @tparam T The type of the optional_ptr
 * @param p_lhs The optional_ptr to compare
 * @return true if the optional_ptr is engaged
 */
template<typename T>
[[nodiscard]] constexpr bool operator!=(optional_ptr<T> const& p_lhs,
                                        std::nullptr_t) noexcept
{
  return p_lhs.has_value();
}

/**
 * @brief Inequality operator between nullptr and optional_ptr
 *
 * nullptr does not equal an optional_ptr if it's engaged.
 *
 * @tparam T The type of the optional_ptr
 * @param p_rhs The optional_ptr to compare
 * @return true if the optional_ptr is engaged
 */
template<typename T>
[[nodiscard]] constexpr bool operator!=(std::nullptr_t,
                                        optional_ptr<T> const& p_rhs) noexcept
{
  return p_rhs.has_value();
}

/**
 * @brief A construction token that can only be created by make_strong_ptr
 *
 * Make the first parameter of your class's constructor(s) in order to limit
 * that constructor to only be used via `make_strong_ptr`.
 */
class strong_ptr_only_token
{
private:
  strong_ptr_only_token() = default;

  template<class U, typename... Args>
  friend constexpr strong_ptr<U> make_strong_ptr(std::pmr::memory_resource*,
                                                 Args&&...);
};

/**
 * @brief Factory function to create a strong_ptr with automatic construction
 * detection
 *
 * This is the primary way to create a new strong_ptr.
 *
 * To create a strong_ptr, you must provide a `std::pmr::memory_resource`.
 * That resource must outlive all strong_ptrs created by it. To uphold the
 * guarantees of strong_ptr, the `std::pmr::memory_resource` MUST terminate the
 * application if it is destroyed before all of its allocated memory has been
 * freed. This requirement prevents use-after-free errors.
 *
 * The function performs the following operations:
 *
 * 1. Allocates memory for both the object and its control block together
 * 2. Detects at compile time if the type expects a strong_ptr_only_token
 * 3. Constructs the object with appropriate parameters
 * 4. Initializes enable_strong_from_this support if the type inherits from it
 * 5. Returns a strong_ptr managing the newly created object
 *
 * **Token-based construction**: If a class constructor takes
 * `strong_ptr_only_token` as its first parameter, this function automatically
 * provides that token, ensuring the class can only be constructed via
 * make_strong_ptr.
 *
 * **Normal construction**: For regular classes, construction proceeds normally
 * with the provided arguments.
 *
 * **enable_strong_from_this support**: If the constructed type inherits from
 * `enable_strong_from_this<T>`, the weak reference is automatically initialized
 * to enable `strong_from_this()` and `weak_from_this()` functionality.
 *
 * Example usage:
 *
 * ```cpp
 * // Token-protected class (can only be created via make_strong_ptr)
 * class protected_driver {
 * public:
 *   protected_driver(strong_ptr_only_token, driver_config config);
 * };
 * auto driver = make_strong_ptr<protected_driver>(allocator, config{});
 *
 * // Regular class
 * class regular_class {
 * public:
 *   regular_class(int value);
 * };
 * auto obj = make_strong_ptr<regular_class>(allocator, 42);
 *
 * // Class with enable_strong_from_this support
 * class self_aware : public enable_strong_from_this<self_aware> {
 * public:
 *   self_aware(std::string name);
 *   void register_callback() {
 *     auto self = strong_from_this(); // This works automatically
 *   }
 * };
 * auto obj = make_strong_ptr<self_aware>(allocator, "example");
 * ```
 *
 * @tparam T The type of object to create
 * @tparam Args Types of arguments to forward to the constructor
 * @param p_memory_resource the memory resource used to allocate memory for the
 * strong_ptr. The memory resource must call `std::terminate` if it is destroyed
 * without all of its memory being freed.
 * @param p_args Arguments to forward to the constructor
 * @return A strong_ptr managing the newly created object
 * @throws Any exception thrown by the object's constructor
 * @throws std::bad_alloc if memory allocation fails
 */
template<class T, typename... Args>
[[nodiscard]] constexpr strong_ptr<T> make_strong_ptr(
  std::pmr::memory_resource* p_memory_resource,
  Args&&... p_args)
{
  using rc_t = rc<T>;

  rc_t* obj = nullptr;

  std::pmr::polymorphic_allocator<> allocator(p_memory_resource);
  if constexpr (std::is_constructible_v<T, strong_ptr_only_token, Args...>) {
    // Type expects token as first parameter
    obj = allocator.new_object<rc_t>(p_memory_resource,
                                     strong_ptr_only_token{},
                                     std::forward<Args>(p_args)...);
  } else {
    // Normal type, construct without token
    obj = allocator.new_object<rc_t>(p_memory_resource,
                                     std::forward<Args>(p_args)...);
  }

  strong_ptr<T> result(&obj->m_info, &obj->m_object);

  // Initialize enable_strong_from_this if the type inherits from it
  if constexpr (std::is_base_of_v<enable_strong_from_this<T>, T>) {
    result->init_weak_this(obj);
  }

  return result;
}
}  // namespace mem::inline v1

namespace async::inline v0 {

using u8 = std::uint8_t;
using byte = std::uint8_t;
using usize = std::size_t;

enum class blocked_by : u8
{
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

/**
 * @brief The data type for sleep time duration
 *
 */
using sleep_duration = std::chrono::nanoseconds;

/**
 * @brief
 *
 */
class scheduler
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

mem::strong_ptr<scheduler> noop_scheduler()
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

class context
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
      allocator.deallocate_object<byte>(m_stack.data(), m_stack.size());
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

  using context_state = std::variant<usize, blocked_by>;
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

class context_token
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
  constexpr context_token& operator=(std::nullptr_t) noexcept
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

template<typename T>
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
template<typename T>
using future_state = std::variant<monostate_or<T>,
                                  std::coroutine_handle<>,
                                  cancelled_state,
                                  std::exception_ptr>;

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

template<typename T>
class promise : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<promise<T>>;

  friend class future<T>;

  // Add regular delete operators for normal coroutine destruction
  static constexpr void operator delete(void*) noexcept
  {
  }

  static constexpr void operator delete(void*, std::size_t) noexcept
  {
  }

  constexpr final_awaiter<promise<T>> final_suspend() noexcept
  {
    return {};
  }

  void unhandled_exception() noexcept
  {
    *m_future_state = std::current_exception();
  }

  future<T> get_return_object() noexcept;

  template<typename U>
  void return_value(U&& p_value) noexcept
    requires std::is_constructible_v<T, U&&>
  {
    // set future to its awaited T value
    m_future_state->template emplace<T>(std::forward<U>(p_value));
  }

  ~promise()
  {
    m_context->deallocate(m_frame_size);
  }

protected:
  future_state<T>* m_future_state;
  usize m_frame_size;
};

template<>
class promise<void> : public promise_base
{
public:
  using promise_base::promise_base;  // Inherit constructors
  using promise_base::operator new;
  using our_handle = std::coroutine_handle<promise<void>>;

  friend class future<void>;

  // Add regular delete operators for normal coroutine destruction
  static constexpr void operator delete(void*) noexcept
  {
  }

  static constexpr void operator delete(void*, std::size_t) noexcept
  {
  }

  constexpr final_awaiter<promise<void>> final_suspend() noexcept
  {
    return {};
  }

  future<void> get_return_object() noexcept;

  void unhandled_exception() noexcept
  {
    *m_future_state = std::current_exception();
  }

  void return_void() noexcept
  {
    *m_future_state = std::monostate{};
  }

  ~promise()
  {
    m_context->deallocate(m_frame_size);
  }

protected:
  future_state<void>* m_future_state;
  usize m_frame_size;
};

template<typename T>
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
      return m_operation.done();
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
      if (auto* ex = std::get_if<std::exception_ptr>(&m_operation.m_state))
        [[unlikely]] {
        std::rethrow_exception(*ex);
      }
      return std::get<T>(m_operation.m_state);
    }

    constexpr void await_resume() const
      requires(std::is_void_v<T>)
    {
      if (auto* ex = std::get_if<std::exception_ptr>(&m_operation.m_state))
        [[unlikely]] {
        std::rethrow_exception(*ex);
      }
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
future<T> promise<T>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<T>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  // Retrieve the frame size and store it for deallocation on destruction
  m_frame_size = m_context->last_allocation_size();
  return future<T>{ handle };
}

inline future<void> promise<void>::get_return_object() noexcept
{
  using future_handle = std::coroutine_handle<promise<void>>;
  auto handle = future_handle::from_promise(*this);
  m_context->active_handle(handle);
  // Retrieve the frame size and store it for deallocation on destruction
  m_frame_size = m_context->last_allocation_size();
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
      m_stack_index = 0;
      return;
    }
    index = continuation;
  }
}
}  // namespace async::inline v0

struct test_scheduler
  : public async::scheduler
  , mem::enable_strong_from_this<test_scheduler>
{
  int sleep_count = 0;
  async::context* sync_context = nullptr;
  bool io_block = false;

  test_scheduler(mem::strong_ptr_only_token)
  {
  }

private:
  void do_schedule([[maybe_unused]] async::context& p_context,
                   [[maybe_unused]] async::blocked_by p_block_state,
                   [[maybe_unused]] async::scheduler::block_info
                     p_block_info) noexcept override
  {

    switch (p_block_state) {
      case async::blocked_by::time: {
        if (std::holds_alternative<std::chrono::nanoseconds>(p_block_info)) {
          sleep_count++;
        }
        break;
      }
      case async::blocked_by::sync: {
        if (std::holds_alternative<async::context*>(p_block_info)) {
          auto* context = std::get<async::context*>(p_block_info);
          sync_context = context;
        }
        break;
      }
      case async::blocked_by::io: {
        io_block = true;
        break;
      }
      case async::blocked_by::nothing: {
        break;
      }
      default: {
        break;
      }
    }
  }

  std::pmr::memory_resource& do_get_allocator() noexcept override
  {
    return *strong_from_this().get_allocator();
  }
};

// Quick Bench: https://quick-bench.com/
// Compiler flags: -std=c++23 -O3 -DNDEBUG
//
// Include your de-moduled async_context code above this section
// ============================================================================

// ============================================================================
// BENCHMARKS
// ============================================================================

// Prevent compiler from optimizing away the result
template<typename T>
void escape(T&& val)
{
  benchmark::DoNotOptimize(val);
}

#if 0
// ----------------------------------------------------------------------------
// 1. BASELINE: Direct returns, 3 levels deep
// ----------------------------------------------------------------------------

__attribute__((noinline)) int direct_level3(int x)
{
  return x * 2;
}

__attribute__((noinline)) int direct_level2(int x)
{
  return direct_level3(x) + 1;
}

__attribute__((noinline)) int direct_level1(int x)
{
  return direct_level2(x) + 1;
}

static void BM_DirectReturn(benchmark::State& state)
{
  int input = 42;
  for (auto _ : state) {
    int result = direct_level1(input);
    escape(result);
  }
}
BENCHMARK(BM_DirectReturn);
#endif

// ----------------------------------------------------------------------------
// 2. VIRTUAL CALLS: Indirect function calls, 3 levels deep
// ----------------------------------------------------------------------------

struct VirtualBase
{
  virtual int compute(int x) = 0;
  virtual ~VirtualBase() = default;
};

struct VirtualLevel3 : VirtualBase
{
  int compute(int x) override
  {
    return x * 2;
  }
};

struct VirtualLevel2 : VirtualBase
{
  VirtualLevel2(VirtualBase* next)
    : m_next(next)
  {
  }
  int compute(int x) override
  {
    return m_next->compute(x) + 1;
  }
  VirtualBase* m_next;
};

struct VirtualLevel1 : VirtualBase
{
  VirtualLevel1(VirtualBase* next)
    : m_next(next)
  {
  }
  int compute(int x) override
  {
    return m_next->compute(x) + 1;
  }
  VirtualBase* m_next;
};

static void BM_VirtualCall(benchmark::State& state)
{
  VirtualLevel3 level3;
  VirtualLevel2 level2(&level3);
  VirtualLevel1 level1(&level2);
  VirtualBase* base = &level1;

  int input = 42;
  for (auto _ : state) {
    int result = base->compute(input);
    escape(result);
  }
}
BENCHMARK(BM_VirtualCall);

// ----------------------------------------------------------------------------
// 3. FUTURE SYNC: Non-coroutine functions returning future<int>, 3 levels deep
//    These functions directly construct future with a value (no coroutine)
// ----------------------------------------------------------------------------

__attribute__((noinline)) async::future<int> sync_future_level3(async::context&,
                                                                int x)
{
  return x * 2;  // Direct construction, no coroutine frame
}

__attribute__((noinline)) async::future<int> sync_future_level2(
  async::context& ctx,
  int x)
{
  auto f = sync_future_level3(ctx, x);
  return f.sync_wait() + 1;
}

__attribute__((noinline)) async::future<int> sync_future_level1(
  async::context& ctx,
  int x)
{
  auto f = sync_future_level2(ctx, x);
  return f.sync_wait() + 1;
}

auto scheduler =
  mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());

static void BM_FutureSyncReturn(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = sync_future_level1(ctx, input);
    int result = f.sync_wait();
    escape(result);
  }
}
BENCHMARK(BM_FutureSyncReturn);

// ----------------------------------------------------------------------------
// 4. FUTURE COROUTINE: Actual coroutines returning future<int>, 3 levels deep
//    These are real coroutines that suspend and resume
// ----------------------------------------------------------------------------

__attribute__((noinline)) async::future<int> coro_level3(async::context&, int x)
{
  co_return x * 2;
}

__attribute__((noinline)) async::future<int> coro_level2(async::context& ctx,
                                                         int x)
{
  int val = co_await coro_level3(ctx, x);
  co_return val + 1;
}

__attribute__((noinline)) async::future<int> coro_level1(async::context& ctx,
                                                         int x)
{
  int val = co_await coro_level2(ctx, x);
  co_return val + 1;
}

static void BM_FutureCoroutine(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = coro_level1(ctx, input);
    int result = f.sync_wait();
    escape(result);
  }
}
BENCHMARK(BM_FutureCoroutine);

// ----------------------------------------------------------------------------
// 5. FUTURE SYNC AWAIT: Sync futures co_awaited inside a coroutine
//    Tests the await_ready() -> await_resume() fast path
// ----------------------------------------------------------------------------

__attribute__((noinline)) async::future<int> sync_in_coro_level3(
  async::context&,
  int x)
{
  return x * 2;  // Sync return
}

__attribute__((noinline)) async::future<int> sync_in_coro_level2(
  async::context& ctx,
  int x)
{
  int val = co_await sync_in_coro_level3(ctx, x);  // Should hit fast path
  co_return val + 1;
}

__attribute__((noinline)) async::future<int> sync_in_coro_level1(
  async::context& ctx,
  int x)
{
  int val = co_await sync_in_coro_level2(ctx, x);
  co_return val + 1;
}

static void BM_FutureSyncAwait(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = sync_in_coro_level1(ctx, input);
    int result = f.sync_wait();
    escape(result);
  }
}
BENCHMARK(BM_FutureSyncAwait);

// ----------------------------------------------------------------------------
// 6. MIXED: Coroutine at top, sync futures below
//    Common pattern: driver coroutine calling sync helper functions
// ----------------------------------------------------------------------------

__attribute__((noinline)) async::future<int> mixed_sync_level3(async::context&,
                                                               int x)
{
  return x * 2;
}

__attribute__((noinline)) async::future<int> mixed_sync_level2(
  async::context& ctx,
  int x)
{
  return mixed_sync_level3(ctx, x).sync_wait() + 1;
}

__attribute__((noinline)) async::future<int> mixed_coro_level1(
  async::context& ctx,
  int x)
{
  // Top level is coroutine, calls sync functions
  int val = co_await mixed_sync_level2(ctx, x);
  co_return val + 1;
}

static void BM_FutureMixed(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = mixed_coro_level1(ctx, input);
    int result = f.sync_wait();
    escape(result);
  }
}
BENCHMARK(BM_FutureMixed);

// ----------------------------------------------------------------------------
// 7. VOID COROUTINES: Test void return path overhead
// ----------------------------------------------------------------------------

__attribute__((noinline)) async::future<void> void_coro_level3(async::context&,
                                                               int& out,
                                                               int x)
{
  out = x * 2;
  co_return;
}

__attribute__((noinline)) async::future<void>
void_coro_level2(async::context& ctx, int& out, int x)
{
  co_await void_coro_level3(ctx, out, x);
  out += 1;
  co_return;
}

__attribute__((noinline)) async::future<void>
void_coro_level1(async::context& ctx, int& out, int x)
{
  co_await void_coro_level2(ctx, out, x);
  out += 1;
  co_return;
}

static void BM_FutureVoidCoroutine(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  int output = 0;
  for (auto _ : state) {
    auto f = void_coro_level1(ctx, output, input);
    f.sync_wait();
    escape(output);
  }
}
BENCHMARK(BM_FutureVoidCoroutine);

// ---------------------------------------------------------------------------
// 8. VIRTUAL CALLS – variant return type
// ---------------------------------------------------------------------------

struct VirtualBaseVariant
{
  // Return a variant that holds the integer result.
  virtual async::future_state<int> compute(int x) = 0;
  virtual ~VirtualBaseVariant() noexcept = default;
};

struct VirtualLevel3Variant : VirtualBaseVariant
{
  async::future_state<int> compute(int x) override
  {
    // For this benchmark we never use the coroutine‑handle or the
    // cancelled_state – only the normal value.
    return async::future_state<int>{ x * 2 };
  }
};

struct VirtualLevel2Variant : VirtualBaseVariant
{
  explicit VirtualLevel2Variant(VirtualBaseVariant* next) noexcept
    : m_next(next)
  {
  }

  async::future_state<int> compute(int x) override
  {
    // Forward the call to the next level and add 1.
    auto res = m_next->compute(x);
    // `res` is a variant; we only care about the int case here.
    // The overhead of `std::get<int>(res)` is what we want to
    // measure.  If the value case is not present we simply return it.
    if (auto* p = std::get_if<int>(&res)) {
      *p += 1;
    }
    return res;
  }

  VirtualBaseVariant* m_next;
};

struct VirtualLevel1Variant : VirtualBaseVariant
{
  explicit VirtualLevel1Variant(VirtualBaseVariant* next) noexcept
    : m_next(next)
  {
  }

  async::future_state<int> compute(int x) override
  {
    auto res = m_next->compute(x);
    if (auto* p = std::get_if<int>(&res)) {
      *p += 1;
    }
    return res;
  }

  VirtualBaseVariant* m_next;
};

static void BM_VirtualCallVariant(benchmark::State& state)
{
  VirtualLevel3Variant level3;
  VirtualLevel2Variant level2(&level3);
  VirtualLevel1Variant level1(&level2);
  VirtualBaseVariant* base = &level1;

  int input = 42;
  for (auto _ : state) {
    // The returned variant is immediately inspected to extract the
    // integer value – this mirrors what your coroutine code does
    // when it needs to "resume" a finished future.
    auto res = base->compute(input);
    int value;
    if (auto* p = std::get_if<int>(&res)) {
      value = *p;
    } else if (auto* p = std::get_if<std::exception_ptr>(&res)) {
      // In the benchmark we never throw, but this makes the
      // code more realistic.
      std::rethrow_exception(*p);
    } else {
      // Should never happen in this test.
      value = 0;
    }
    escape(value);
  }
}
BENCHMARK(BM_VirtualCallVariant);
// NOLINTEND(readability-identifier-naming)
