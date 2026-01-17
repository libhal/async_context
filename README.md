# async_context

[![✅ CI](https://github.com/libhal/async_context/actions/workflows/ci.yml/badge.svg)](https://github.com/libhal/async_context/actions/workflows/ci.yml)
[![Standard](https://img.shields.io/badge/C%2B%2B-23-C%2B%2B23?logo=c%2B%2B&color=00599C&style=flat)](https://isocpp.org/std/the-standard)
[![GitHub stars](https://img.shields.io/github/stars/libhal/async_context.svg)](https://github.com/libhal/async_context/stargazers)
[![GitHub forks](https://img.shields.io/github/forks/libhal/async_context.svg)](https://github.com/libhal/async_context/network)
[![GitHub issues](https://img.shields.io/github/issues/libhal/async_context.svg)](https://github.com/libhal/async_context/issues)

A lightweight, C++23 coroutine library for embedded systems and
resource-constrained environments. Built with stack-based allocation to avoid
heap usage and designed to fit within a single cache line for optimal
performance.

> [!CAUTION]
>
> 🚧 This project is still under construction! 🚧

```C++
#include <cassert>

#include <chrono>
#include <coroutine>
#include <print>
#include <thread>

import async_context;

using namespace std::chrono_literals;

// Simulates reading sensor data with I/O delay
async::future<int> read_sensor(async::context& ctx, std::string_view p_name)
{
  std::println("['{}': Sensor] Starting read...", p_name);
  co_await ctx.block_by_io();  // Simulate I/O operation
  std::println("['{}': Sensor] Read complete: 42", p_name);
  co_return 42;
}

// Processes data with computation delay
async::future<int> process_data(async::context& ctx,
                                std::string_view p_name,
                                int value)
{
  std::println("['{}': Process] Processing {}...", p_name, value);
  co_await 10ms;  // Simulate processing time
  int result = value * 2;
  std::println("['{}': Process] Result: {}", p_name, result);
  co_return result;
}

// Writes result with I/O delay
async::future<void> write_actuator(async::context& ctx,
                                   std::string_view p_name,
                                   int value)
{
  std::println("['{}': Actuator] Writing {}...", p_name, value);
  co_await ctx.block_by_io();
  std::println("['{}': Actuator] Write complete!", p_name);
}

// Coordinates the full pipeline
async::future<void> sensor_pipeline(async::context& ctx,
                                    std::string_view p_name)
{
  std::println("Pipeline '{}' starting...", p_name);

  int sensor_value = co_await read_sensor(ctx, p_name);
  int processed = co_await process_data(ctx, p_name, sensor_value);
  co_await write_actuator(ctx, p_name, processed);

  std::println("Pipeline '{}' complete!\n", p_name);
}

int main()
{
  // Create context and add them to the scheduler
  basic_context<8192> ctx1(scheduler);
  basic_context<8192> ctx2(scheduler);

  // Run two independent pipelines concurrently
  auto pipeline1 = sensor_pipeline(ctx1, "🌟 System 1");
  auto pipeline2 = sensor_pipeline(ctx2, "🔥 System 2");

  // Round robin between each context
  while (true) {
   bool all_done = true;
   for (auto& ctx : std::to_array({&ctx1, &ctx2}) {
     if (not ctx->done()) {
       all_done = false;
       if (ctx->state() == async::blocked_by::nothing) {
         ctx->resume();
       }
       if (ctx->state() == async::blocked_by::time) {
         std::this_thread::sleep(ctx.pending_delay());
         ctx.unblock();
       }
     }
   }
   if (all_done) {
     break;
   }
  }

  assert(pipeline1.done());
  assert(pipeline2.done());

  std::println("Both pipelines completed successfully!");
  return 0;
}
```

Output:

```
Pipeline '🌟 System 1' starting...
['🌟 System 1': Sensor] Starting read...
Pipeline '🔥 System 2' starting...
['🔥 System 2': Sensor] Starting read...
['🌟 System 1': Sensor] Read complete: 42
['🌟 System 1': Process] Processing 42...
['🔥 System 2': Sensor] Read complete: 42
['🔥 System 2': Process] Processing 42...
['🌟 System 1': Process] Result: 84
['🌟 System 1': Actuator] Writing 84...
['🔥 System 2': Process] Result: 84
['🔥 System 2': Actuator] Writing 84...
['🌟 System 1': Actuator] Write complete!
Pipeline '🌟 System 1' complete!

['🔥 System 2': Actuator] Write complete!
Pipeline '🔥 System 2' complete!

Both pipelines completed successfully!
```

## Features

- **Stack-based coroutine allocation** - No heap allocations; coroutine frames are allocated from a user-provided stack buffer
- **Cache-line optimized** - Context object fits within `std::hardware_constructive_interference_size` (typically 64 bytes)
- **Blocking state tracking** - Built-in support for time, I/O, sync, and external blocking states
- **Scheduler integration** - Virtual `do_schedule()` method allows custom scheduler implementations
- **Proxy contexts** - Support for supervised coroutines with timeout capabilities
- **Exception propagation** - Proper exception handling through the coroutine chain
- **Cancellation support** - Clean cancellation with RAII-based resource cleanup

## Requirements

- C++23 compiler with coroutine support
- Tested with Clang 20+
- Usage of C++20 modules

## Stack-Based Allocation

Unlike typical coroutine implementations that allocate frames on the heap,
`async_context` uses a stack-based allocation scheme. Each context owns a
contiguous buffer of memory that grows upward as coroutines are called.

### How Allocation Works

```ascii
┌─────────────────────────────┐ Address 0
│  &context::m_stack_pointer  │
├─────────────────────────────┤
│     Coroutine Frame A       │
│     (promise + locals)      │
|           (96 B)            │
├─────────────────────────────┤
│  &context::m_stack_pointer  │
├─────────────────────────────┤
│     Coroutine Frame B       │
|           (192 B)           │
│     (promise + locals)      │
│                             │
│                             │
│                             │
├─────────────────────────────┤
│  &context::m_stack_pointer  │
├─────────────────────────────┤
│     Coroutine Frame C       │
|           (128 B)           │
│     (promise + locals)      │
│                             │
├─────────────────────────────┤
│        Unused Memory        │ <-- context::m_stack_pointer
│                             │
│                             │
│                             │
│                             │
│                             │
│                             │
│                             │
└─────────────────────────────┘ Address N (bytes of stack memory)
```

1. **Allocation**: When a coroutine is created, the promise's `operator new`
   requests memory from the context. The context:
   - Stores the address of `m_stack_pointer` at the current position
   - Returns the next address as the coroutine frame location
   - Advances `m_stack_pointer` past the allocated frame

2. **Deallocation**: When a coroutine completes, `operator delete`:
   - Reads the stored `&m_stack_pointer` from just before the frame
   - Resets `m_stack_pointer` back to that position

This creates a strict LIFO stack where coroutines must complete in reverse
order of their creation, which naturally matches how `co_await` chains
work.

### Benefits

- **No heap allocation on frame creation**: Ideal for embedded systems without
  dynamic memory
- **Deterministic**: Memory usage is bounded by the stack buffer size
- **Cache-friendly**: Coroutine frames are contiguous in memory
- **Fast**: Simple pointer arithmetic instead of malloc/free

## Core Types

### `async::context`

The base context class that manages coroutine execution and memory. Derived classes must:

1. Provide stack memory via `initialize_stack_memory()`, preferably within the
   constructor.
2. Implement `do_schedule()` to handle blocking state notifications

### `async::future<T>`

A coroutine return type containing either a value, asynchronous operation, or
an `std::exception_ptr`. If this object is contains a coroutine handle, then it
the future must be resumed until the future object is converted into the value
of type `T`.

- Synchronous returns (no coroutine frame allocation)
- `co_await` for composing asynchronous operations
- `co_return` for returning values
- Move semantics (non-copyable)

### `async::task`

An alias for `async::future<void>` - an async operation with no return value.

### `async::blocked_by`

An enum describing what a coroutine is blocked by:

- `nothing` - Ready to run
- `io` - Blocked by I/O operation
- `sync` - Blocked by resource contention (mutex, semaphore)
- `external` - Blocked by external coroutine system
- `time` - Blocked until a duration elapses

The state of this can be found from the `async::context::state()`. All states
besides time are safe to resume at any point. If a context has been blocked by
time, then it must defer calling resume until that time has elapsed.

## Usage

### Basic Coroutine

```cpp
import async_context;

async::future<int> compute(async::context& p_ctx) {
    co_return 42;
}
```

### Awaiting Time

```cpp
async::future<void> delay_example(async::context& p_ctx) {
    using namespace std::chrono_literals;
    co_await 100ms;  // Request the scheduler resume this coroutine >= 100ms
    co_return;
}
```

### Awaiting I/O

```cpp
async::future<void> io_example(async::context& p_ctx) {
    dma_controller.on_completion([&p_ctx]() {
      p_ctx.unblock();
    });

    // Start DMA transaction...

    while (!dma_complete) {
        co_await p_ctx.block_by_io();
    }
    co_return;
}
```

Please note that this coroutine has a loop where it continually reports that
its blocked by IO. It is important that any coroutine blocking by IO check if
the IO has completed before proceeding. If not, it must
`co_await ctx.block_by_io();` at some point to give control back to the resumer.

### Composing Coroutines

```cpp
async::future<int> inner(async::context& p_ctx) {
    co_return 10;
}

async::future<int> outer(async::context& p_ctx) {
    int value = co_await inner(p_ctx);
    co_return value * 2;
}
```

### Custom Context Implementation

```cpp
class my_context : public async::context {
public:
    std::array<async::uptr, 1024> m_stack{};

    my_context() {
        initialize_stack_memory(m_stack);
    }
    ~my_context() {
        // ‼️ The most derived context must call cancel in its destructor
        cancel();
        // If memory was allocated, deallocate it here...
    }

private:
    void do_schedule(async::blocked_by p_state,
                     async::block_info p_info) noexcept override {
        // Notify your scheduler of state changes
    }
};
```

#### Initialization

In order to create a usable custom context, the stack memory must be
initialized with a call to `initialize_stack_memory(span)` with a span to the
memory for the stack. There is no requirements of where this memory comes from
except that it be a valid source. Such sources can be array thats member of
this object, dynamically allocated memory that the context has sole ownership
of, or it can point to statically allocated memory that it has sole control and
ownership over.

#### Destruction

The custom context must call `cancel()` before deallocating the stack memory.
Once cancel completes, the stack memory may be deallocated.

### Using basic_context with sync_wait

```cpp
class simple_context : public async::basic_context {
public:
    std::array<async::uptr, 8192> m_stack{};

    simple_context() {
        initialize_stack_memory(m_stack);
    }

    ~simple_context() {
        cancel();
    }
};

simple_context ctx;
auto future = my_coroutine(ctx);
ctx.sync_wait([](async::sleep_duration p_sleep_time) {
    std::this_thread::sleep_for(p_sleep_time);
});
```

## Exception Handling

Exceptions thrown in coroutines are propagated through the coroutine chain
until it reaches the top level coroutine. When the top level is reached, the
exception will be thrown from a call to `.resume()`.

```cpp
async::future<void> may_throw(async::context& p_ctx) {
  throw std::runtime_error("error");
  co_return;
}

async::future<void> just_calls(async::context& p_ctx) {
  co_await may_throw(p_ctx);
  co_return;
}

simple_context ctx;
auto future = may_throw(ctx);

try {
  future.resume();
} catch (const std::runtime_error& e) {
  // Handle exception
}
```

### Avoid operation stacking

Operation stacking is when you load an additional operation into a coroutine's
stack memory before finishing the previous operation. This results in a memory
leak where the previous coroutines frame is no longer accessible and cannot be
deallocated, permanently reducing the memory of the context and preserving the
lifetime of the objects held within. It is UB to allow the context to be
destroyed at this point.

```cpp
my_context ctx;
auto future1 = async_op1(ctx); // ✅ Okay, may create some objects on its stack
auto future2 = async_op2(ctx); // ❌ Memory leak! Don't do this
// UB to allow ctx to be destroyed at this point 😱
```

### Proxy Context for Timeouts

```cpp
async::future<int> supervised(async::context& p_ctx) {
    auto proxy = async::proxy_context::from(p_ctx);
    auto child_future = child_coroutine(proxy);

    int timeout = 10;
    while (!child_future.done() && timeout-- > 0) {
        child_future.resume();
        co_await std::suspend_always{};
    }

    if (timeout <= 0) {
        throw timed_out();
    }
    co_return child_future.value();
}
```

`async::proxy_context::from()`: consumes the rest of the stack memory of the
context for itself. The original context's stack memory will be clamped to
where it was when it called the `supervised` function.  The stack memory is
restored to the original context, once the proxy is destroyed. This prevents
the context from being used again and overwriting the memory of the stack.

Each coroutine frame is allowed to have at most 1 proxy on its stack. This
allows for top down chaining of proxies as shown below. When a proxy blocks by
something, that blocking state is communicated to the original context and its
schedule function is executed. When the original context is resumed, it will
execute from its active coroutine which contains a proxy. That coroutine may
check for timeouts and resume its supervised future. Then that future may have
yet another proxy which performs the same work of timeout detection as before
and resumes the future it has supervision for. This continues on until the
bottom is reached or a coroutine decides to cancel its future or exit via
exception or normal return path.

This naturally gives the top most supervising coroutine priority to determine
if its time frame has expired.

```ascii
┌─────────────────────────────┐ Address 0
│  &context::m_stack_pointer  │
├─────────────────────────────┤
│     Coroutine Frame A       │
│     (promise + locals)      │
│           (96 B)            │
├─────────────────────────────┤
│  &context::m_stack_pointer  │
├─────────────────────────────┤
│     Coroutine Frame B       │
│           (64 B)            │
│     (promise + locals)      │
├─────────────────────────────┤
│  &context::m_stack_pointer  │ <-- Origin Stack ends here
├─────────────────────────────┤    (m_stack_pointer shrunk to here)
│     Coroutine Frame C       │
│           (128 B)           │    Proxy-1 Stack begins
│     (promise + locals)      │
│                             │
├─────────────────────────────┤
│  &proxy1::m_stack_pointer   │
├─────────────────────────────┤
│     Coroutine Frame D       │
│           (80 B)            │
│     (promise + locals)      │
├─────────────────────────────┤
│  &proxy1::m_stack_pointer   │
├─────────────────────────────┤
│     Coroutine Frame E       │
│           (72 B)            │
│     (promise + locals)      │
├─────────────────────────────┤
│  &proxy1::m_stack_pointer   │ <-- Proxy-1 Stack ends here
├─────────────────────────────┤
│     Coroutine Frame F       │
│           (96 B)            │    Proxy-2 Stack begins
│     (promise + locals)      │
├─────────────────────────────┤
│  &proxy2::m_stack_pointer   │
├─────────────────────────────┤
│     Coroutine Frame G       │
│           (144 B)           │
│     (promise + locals)      │
│                             │
├─────────────────────────────┤
│  &proxy2::m_stack_pointer   │ <-- Proxy-2 Stack ends here
├─────────────────────────────┤
│     Coroutine Frame H       │
│           (88 B)            │    Proxy-3 Stack begins
│     (promise + locals)      │
├─────────────────────────────┤
│  &proxy3::m_stack_pointer   │ <-- Proxy-3 current position
├─────────────────────────────┤
│        Unused Memory        │
│                             │
│                             │
│                             │
│                             │
│                             │
│                             │
└─────────────────────────────┘ Address N (bytes of stack memory)
```

## Creating the package

Before getting started, if you haven't used libhal before, follow the
[Getting Started](https://libhal.github.io/latest/getting_started/) guide.

To create the library package call:

```bash
conan create . -pr hal/tc/llvm-20 -pr hal/os/mac --version=<insert-version>
```

Replace `mac` with `linux` or `windows` if that is what you are building on.

This will build and run unit tests, benchmarks, and a test package to confirm
that the package was built correctly.

To run tests on their own:

```bash
./build/Release/async_context_tests
```

To run the benchmarks on their own:

```bash
./build/Release/async_benchmark
```


Within the [`CMakeList.txt`](./CMakeLists.txt), you can disable unit test or benchmarking by setting the following to `OFF`:

```cmake
set(BUILD_UNIT_TESTS OFF)
set(BUILD_BENCHMARKS OFF)
```

## License

Apache License 2.0 - See [LICENSE](LICENSE) for details.

Copyright 2024 - 2025 Khalil Estell and the libhal contributors
