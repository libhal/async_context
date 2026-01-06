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

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>

#include <array>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <memory_resource>
#include <new>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

import async_context;

// Quick Bench: https://quick-bench.com/
// Compiler flags: -std=c++23 -O3 -DNDEBUG
//
// Include your de-moduled async_context code above this section
// ============================================================================

// ============================================================================
// BENCHMARKS
// ============================================================================

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

int (*f_ptr)(int) = &direct_level1;

static void bm_function_pointer_call(benchmark::State& state)
{
  int input = 42;
  for (auto _ : state) {
    int result = f_ptr(input);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(f_ptr);
  }
}
BENCHMARK(bm_function_pointer_call);

// ----------------------------------------------------------------------------
// 2.0 VIRTUAL CALLS: Indirect function calls, 3 levels deep
// ----------------------------------------------------------------------------

struct virtual_base
{
  virtual int compute(int x) = 0;
  virtual ~virtual_base() = default;
};

struct virtual_level3 : virtual_base
{
  int compute(int x) override
  {
    return x * 2;
  }
};

struct virtual_level2 : virtual_base
{
  virtual_level2(virtual_base* next)
    : m_next(next)
  {
  }
  int compute(int x) override
  {
    return m_next->compute(x) + 1;
  }
  virtual_base* m_next;
};

struct virtual_level1 : virtual_base
{
  virtual_level1(virtual_base* next)
    : m_next(next)
  {
  }
  int compute(int x) override
  {
    return m_next->compute(x) + 1;
  }
  virtual_base* m_next;
};

static void bm_virtual_call(benchmark::State& state)
{
  virtual_level3 level3;
  virtual_level2 level2(&level3);
  virtual_level1 level1(&level2);
  virtual_base* base = &level1;

  int input = 42;
  for (auto _ : state) {
    int result = base->compute(input);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(bm_virtual_call);

// ---------------------------------------------------------------------------
// 2.1. VIRTUAL CALLS – variant return type
// ---------------------------------------------------------------------------

struct virtual_base_variant
{
  // Return a variant that holds the integer result.
  virtual async::future_state<int> compute(int x) = 0;
  virtual ~virtual_base_variant() noexcept = default;
};

struct virtual_level3_variant : virtual_base_variant
{
  async::future_state<int> compute(int x) override
  {
    // For this benchmark we never use the coroutine‑handle or the
    // cancelled_state – only the normal value.
    return async::future_state<int>{ x * 2 };
  }
};

struct virtual_level2_variant : virtual_base_variant
{
  explicit virtual_level2_variant(virtual_base_variant* next) noexcept
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

  virtual_base_variant* m_next;
};

struct virtual_level1_variant : virtual_base_variant
{
  explicit virtual_level1_variant(virtual_base_variant* next) noexcept
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

  virtual_base_variant* m_next;
};

static void bm_virtual_call_variant(benchmark::State& state)
{
  virtual_level3_variant level3;
  virtual_level2_variant level2(&level3);
  virtual_level1_variant level1(&level2);
  virtual_base_variant* base = &level1;

  int input = 42;
  for (auto _ : state) {
    // The returned variant is immediately inspected to extract the
    // integer value – this mirrors what your coroutine code does
    // when it needs to "resume" a finished future.
    auto res = base->compute(input);
    int value;
    if (auto* p = std::get_if<int>(&res)) {
      value = *p;
    } else if (auto* exception = std::get_if<std::exception_ptr>(&res)) {
      // In the benchmark we never throw, but this makes the
      // code more realistic.
      std::rethrow_exception(*exception);
    } else {
      // Should never happen in this test.
      value = 0;
    }
    benchmark::DoNotOptimize(value);
  }
}
BENCHMARK(bm_virtual_call_variant);

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
struct test_context : public async::context
{
  int sleep_count = 0;
  async::context* sync_context = nullptr;
  bool io_block = false;

  test_context(mem::strong_ptr_only_token)
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

auto scheduler =
  mem::make_strong_ptr<test_context>(std::pmr::new_delete_resource());

static void bm_future_sync_return(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = sync_future_level1(ctx, input);
    int result = f.sync_wait();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(bm_future_sync_return);

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

static void bm_future_coroutine(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = coro_level1(ctx, input);
    int result = f.sync_wait();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(bm_future_coroutine);

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

static void bm_future_sync_await(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = sync_in_coro_level1(ctx, input);
    int result = f.sync_wait();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(bm_future_sync_await);

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

static void bm_future_mixed(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  for (auto _ : state) {
    auto f = mixed_coro_level1(ctx, input);
    int result = f.sync_wait();
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(bm_future_mixed);

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

static void bm_future_void_coroutine(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  int output = 0;
  for (auto _ : state) {
    auto f = void_coro_level1(ctx, output, input);
    f.sync_wait();
    benchmark::DoNotOptimize(f);
    benchmark::DoNotOptimize(output);
  }
}
BENCHMARK(bm_future_void_coroutine);

static void bm_future_void_coroutine_context_resume(benchmark::State& state)
{
  async::context ctx(scheduler, 4096);

  int input = 42;
  int output = 0;
  for (auto _ : state) {
    auto f = void_coro_level1(ctx, output, input);
    while (not f.done()) {
      ctx.resume();
    }
    benchmark::DoNotOptimize(output);
  }
}
BENCHMARK(bm_future_void_coroutine_context_resume);

BENCHMARK_MAIN();
// NOLINTEND(readability-identifier-naming)
