#include <coroutine>

#include <boost/ut.hpp>

import async_context;
import test_utils;

boost::ut::suite<"basics"> basics = []() {
  using namespace boost::ut;

  "sync return"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int expected_return_value = 5;

    unsigned step = 0;
    auto sync_coroutine = [&step](async::context&) -> async::future<int> {
      step = 1;
      return expected_return_value;
    };

    // Exercise
    auto future = sync_coroutine(ctx);

    // Verify
    expect(that % 0 == ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 1 == step);
  };

  "co_return"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int expected_return_value = 5;
    unsigned step = 0;
    auto async_coroutine = [&step](async::context&) -> async::future<int> {
      step = 1;
      co_return expected_return_value;
    };

    // Exercise 1
    auto future = async_coroutine(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start and finish coroutine
    future.resume();

    // Verify 2
    expect(that % 0 == ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 1 == step);
  };

  "suspend then co_return"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int expected_return_value = 1413;
    unsigned step = 0;
    auto async_coroutine = [&step](async::context&) -> async::future<int> {
      step = 1;
      co_await std::suspend_always{};
      step = 2;
      co_return expected_return_value;
    };

    // Exercise 1
    auto future = async_coroutine(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start and suspend coroutine
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3: resume and co_return from coroutine
    future.resume();

    // Verify 3
    expect(that % 0 == ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 2 == step);
  };

  "co_await coroutine"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int expected_return_value = 1413;
    unsigned step = 0;
    auto co2 = [&step](async::context&) -> async::future<int> {
      step = 2;
      co_await std::suspend_always{};
      // skipped as the co1 will immediately resume
      step = 3;
      co_return expected_return_value;
    };
    auto co = [&step, &co2](async::context& p_ctx) -> async::future<int> {
      step = 1;  // skipped as the co2 will immediately start
      co_await co2(p_ctx);
      step = 4;
      co_return expected_return_value;
    };

    // Exercise 1
    auto future = co(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start, enter co_2, start immediately and suspend
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 2 == step);

    // Exercise 3: resume, co_2 co_returns, immediately resumes parent, return
    future.resume();

    // Verify 3
    expect(that % 0 == ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 4 == step);
  };

  "co_await coroutine"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int return_value1 = 1413;
    static constexpr int return_value2 = 4324;
    static constexpr int expected_total = return_value1 + return_value2;

    unsigned step = 0;
    auto co2 = [](async::context&) -> async::future<int> {
      return return_value1;
    };

    auto co = [&step, &co2](async::context& p_ctx) -> async::future<int> {
      step = 1;  // skipped as the co2 will immediately start
      auto val = co_await co2(p_ctx);
      step = 2;
      co_return val + return_value2;
    };

    // Exercise 1
    auto future = co(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start, call co_2, returns value immediately and co_returns
    future.resume();

    // Verify 3
    expect(that % 0 == ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_total == future.value());
    expect(that % 2 == step);
  };
};
