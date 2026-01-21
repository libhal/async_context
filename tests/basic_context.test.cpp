#include <coroutine>

#include <boost/ut.hpp>
#include <thread>

import async_context;
import test_utils;

boost::ut::suite<"basic_context"> basic_context = []() {
  using namespace boost::ut;

  static constexpr auto stack_size = 1024;

  "sync return type void"_test = []() {
    // Setup
    async::basic_context<stack_size> ctx;

    unsigned step = 0;
    auto sync_coroutine = [&step](async::context&) -> async::future<void> {
      step = 1;
      return {};
    };

    // Exercise
    auto future = sync_coroutine(ctx);

    // Verify
    expect(that % 0 == ctx->memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 1 == step);

    expect(that % stack_size == ctx->capacity());
  };

  "sync_wait --> future<int>"_test = []() {
    // Setup
    async::basic_context<1024> ctx;

    auto future = [](async::context&) -> async::future<int> {
      co_return 5;
    }(ctx);

    ctx->sync_wait([](async::sleep_duration p_sleep_duration) {
      std::this_thread::sleep_for(p_sleep_duration);
    });

    // Verify
    expect(that % 0 == ctx->memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 5 == future.value());
    expect(that % stack_size == ctx->capacity());
  };

  "co_await coroutine"_test = []() {
    // Setup
    async::basic_context<stack_size> ctx;

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
    expect(that % 0 < ctx->memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start, enter co_2, start immediately and suspend
    future.resume();

    // Verify 2
    expect(that % 0 < ctx->memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 2 == step);

    // Exercise 3: resume, co_2 co_returns, immediately resumes parent, return
    future.resume();

    // Verify 3
    expect(that % 0 == ctx->memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 4 == step);

    expect(that % stack_size == ctx->capacity());
  };

  "co_await coroutine"_test = []() {
    // Setup
    async::basic_context<stack_size> ctx;

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
    expect(that % 0 < ctx->memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start, call co_2, returns value immediately and co_returns
    future.resume();

    // Verify 3
    expect(that % 0 == ctx->memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_total == future.value());
    expect(that % 2 == step);

    expect(that % stack_size == ctx->capacity());
  };

  "co_await Xms + sync_wait"_test = []() {
    // Setup
    async::basic_context<stack_size> ctx;

    static constexpr int return_value1 = 1413;
    static constexpr int return_value2 = 4324;
    static constexpr int expected_total = return_value1 + return_value2;

    using namespace std::literals;
    unsigned step = 0;
    auto co2 = [](async::context&) -> async::future<int> {
      co_await 100ms;
      co_return return_value1;
    };

    auto co = [&step, &co2](async::context& p_ctx) -> async::future<int> {
      step = 1;  // skipped as the co2 will immediately start
      co_await 44ms;
      auto val = co_await co2(p_ctx);
      co_await 50ms;
      step = 2;
      co_return val + return_value2;
    };
    std::vector<async::sleep_duration> sleep_cycles;

    // Exercise
    auto future = co(ctx);

    ctx->sync_wait([&](async::sleep_duration p_sleep_time) {
      sleep_cycles.push_back(p_sleep_time);
    });

    // Verify
    expect(that % 0 == ctx->memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 2 == step);
    expect(that % expected_total == future.value());
    expect(that % sleep_cycles ==
           std::vector<async::sleep_duration>{ 44ms, 100ms, 50ms });

    expect(that % stack_size == ctx->capacity());
  };
};
