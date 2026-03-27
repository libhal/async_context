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
>
> This document is missing a section about how to create your own scheduler.
> This is a missing feature we plan to add later.
>
> The APIs of this library are not stable and may change at anytime before
> 1.0.0 release.

```C++
#include <chrono>
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

// Unblocks any I/O-blocked context in ctx1 or ctx2 (simulates hardware
// callbacks completing). Runs as a third coroutine alongside the pipelines.
async::future<void> io_unblock_driver(async::context& p_ctx,
                                      async::context& ctx1,
                                      async::context& ctx2)
{
  while (true) {
    if (ctx1.done() && ctx2.done()) {
      co_return;
    }
    if (ctx1.state() == async::blocked_by::io) {
      ctx1.unblock();
    }
    if (ctx2.state() == async::blocked_by::io) {
      ctx2.unblock();
    }
    co_await 1us;
  }
}

int main()
{
  async::inplace_context<512> ctx1;
  async::inplace_context<512> ctx2;
  async::inplace_context<512> driver_ctx;

  // Start the two pipelines and the I/O unblock driver
  auto pipeline1 = sensor_pipeline(ctx1, "🌟 System 1");
  auto pipeline2 = sensor_pipeline(ctx2, "🔥 System 2");
  auto driver    = io_unblock_driver(driver_ctx, ctx1, ctx2);

  // Drive all three contexts to completion.
  // run_until_done sleeps until the nearest time deadline when all contexts
  // are blocked.
  //
  // NOTE: A proper wake function should wake immediately when any context
  // becomes ready, where as this one sleeps until it is awoken.
  async::chrono_clock_adapter<std::chrono::steady_clock> clk;
  async::run_until_done(
    clk,
    [](auto p_wake_time) { std::this_thread::sleep_until(p_wake_time); },
    ctx1, ctx2, driver_ctx);

  std::println("Both pipelines completed successfully!");
  return 0;
}
```

Output:

```text
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
- **Blocking state tracking** - Built-in support for time, I/O, sync, and external blocking states
- **Flexible scheduler integration** - Schedulers can poll context state directly, or register an `unblock_listener` for ISR-safe event notification when contexts become unblocked
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

### `async::unblock_listener`

An interface for receiving notifications when a context becomes unblocked. This
is the primary mechanism for schedulers to efficiently track which contexts are
ready for execution without polling. Implement this interface and register it
with `context::on_unblock()` to be notified when a context transitions to the
unblocked state.

The `on_unblock()` method is called from within `context::unblock()`, which may
be invoked from ISRs, driver completion handlers, or other threads.
Implementations must be ISR-safe and noexcept.

### `async::context`

The base context class that manages coroutine execution and memory. Contexts
are initialized with stack memory via their constructor:

```cpp
std::array<async::stack_word, 1024> my_stack{};
async::context ctx(my_stack);
```

> [!CRITICAL]
> The stack memory MUST outlive the context object. The context does not own or
> copy the stack memory—it only stores a reference to it.

Optionally, contexts can register an `unblock_listener` to be notified of state
changes, or the scheduler can poll the context state directly using `state()`
and `pending_delay()`

### `async::future<T>`

<div align="center">
  <img src="docs/assets/future_states.svg" height="300" alt="async::future's state transition diagram">
  <p>Figure 1. <code>async::future</code> state transition diagram.</p>
</div>

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

## Schedulers

You can import the schedulers by importing `async_context.schedulers`. Importing
`async_context.schedulers` also transitively imports `async_context`.

### `async::clock` (concept)

An instance-based clock concept. Unlike `std::chrono` clocks which require a
static `now()`, `async::clock` requires an instance method so hardware clocks
can be injected as runtime objects. A conforming type must provide:

- `time_point` — the type returned by `now()`
- `duration` — the difference type of two `time_point`s
- `now() const` — returns the current `time_point`
- `time_point` arithmetic: subtraction yields `duration`, addition of
  `duration` yields `time_point`
- `time_point::max()` — sentinel meaning "never wake"

### `async::chrono_clock_adapter<ChronoClock>`

A zero-size adapter that wraps any `std::chrono`-conforming clock (with a
static `now()`) into an `async::clock` (with an instance `now()`). Because it
holds no state, it is always optimized away entirely by the compiler.

```cpp
async::chrono_clock_adapter<std::chrono::steady_clock> clk;
static_assert(async::clock<decltype(clk)>);

auto now = clk.now(); // forwards to std::chrono::steady_clock::now()
```

Use this on hosted platforms. On bare-metal, implement `async::clock` directly
against your hardware timer peripheral.

### `async::run_until_done`

Drives a fixed set of `async::context` objects to completion in a cooperative
scheduling loop. On each iteration it resumes every context that is ready or
whose time deadline has elapsed. When all remaining contexts are blocked, it
calls the user-supplied `p_sleep_until` callable to suspend the CPU until the
nearest deadline.

```cpp
// Without interruptible sleep
async::run_until_done(
  clk,
  [](auto p_wake_time) { std::this_thread::sleep_until(p_wake_time); },
  ctx0, ctx1, ctx2);
```

An overload accepts an `async::unblock_listener` as a third argument. The
listener is registered on every context so that an I/O completion or ISR can
wake `p_sleep_until` early, avoiding unnecessary latency when all contexts are
time-blocked but an I/O event arrives before the deadline.

```cpp
async::run_until_done(
  clk,
  [](auto p_wake_time) {
    // sleep until the deadline OR until woken by the listener below
    platform_sleep_until(p_wake_time);
  },
  async::unblock_listener::from([](async::context& p_ctx) noexcept {
    // called from ISR/thread when any context is unblocked
    platform_wake_from_sleep();
  }),
  ctx0, ctx1, ctx2);
```

Key properties:

- **Stack-allocated scheduler table** — no heap allocation; all internal
  bookkeeping lives on the call stack and is destroyed when the function
  returns, even on exception
- **Listener lifetime safety** — every context's listener registration is
  cleared in the destructor of the internal scheduler entry, so no context
  can hold a dangling pointer after `run_until_done` returns
- **Time-only sleep** — `p_sleep_until` is only called when all contexts are
  time-blocked *and* none are immediately ready; it is never called if work
  remains
- **Exceptions** — any exception that propagates out of a coroutine is
  re-thrown from `run_until_done`; all listener registrations are still
  cleaned up via RAII before the exception escapes

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

### Using `sync_wait()`

```cpp
async::inplace_context<512> ctx;
auto future = my_coroutine(ctx);
ctx.sync_wait([](async::sleep_duration p_sleep_time) {
    std::this_thread::sleep_for(p_sleep_time);
});
```

Replace `std::this_thread::sleep_for(p_sleep_time);` with whatever sleep
function works best for your systems.

For example, for FreeRTOS this could be:

```C++
// Helper function to convert microseconds to FreeRTOS ticks
inline TickType_t us_to_ticks(const std::chrono::microseconds& us) {
    // Convert microseconds to milliseconds (rounding to nearest ms)
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(us).count();
    return pdMS_TO_TICKS(ms);
}
ctx.sync_wait([](async::sleep_duration p_sleep_time) {
    xTaskDelay(us_to_ticks(p_sleep_time));
});
```

Example using a made up hardware timer, to put system into low power mode:

```C++
ctx.sync_wait([timer_smart_ptr](async::sleep_duration p_sleep_time) {
    timer_smart_ptr->schedule(p_sleep_time, []() { /* do something */ });
    hal::cortex_m::wait_for_interrupt();
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

<div align="center">
  <img src="docs/assets/proxy_linkage.svg" height="250" alt="async::proxy_context linkage">
  <p>Figure 2. Illustration of the linkage between <code>async::proxy_context</code> and their parent proxies as well as their connection to the original context.</p>
</div>

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

Copyright 2024 - 2026 Khalil Estell and the libhal contributors
