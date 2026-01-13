#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

boost::ut::suite<"cancellation_tests"> cancellation_tests = []() {
  using namespace boost::ut;
  using namespace std::chrono_literals;

  "Cancellation"_test = []() {
    // Setup
    test_context ctx;

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    auto a = [get_counter,
              &ends_reached](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      std::println("[future cancel] a exited");
      ends_reached++;
      co_return;
    };

    auto b = [a, get_counter, &ends_reached](
               async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("[future cancel] b exited");
      ends_reached++;
      co_return;
    };

    auto c = [b, get_counter, &ends_reached](
               async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("[future cancel] c exited");
      ends_reached++;
      co_return;
    };

    {
      expect(count == std::make_pair<int, int>(0, 0))
        << "count is {" << count.first << ", " << count.second << "}\n";
      expect(that % ends_reached == 0);

      auto future = c(ctx);

      expect(count == std::make_pair<int, int>(0, 0))
        << "count is {" << count.first << ", " << count.second << "}\n";
      expect(that % ends_reached == 0);

      future.resume();

      expect(count == std::make_pair<int, int>(3, 0))
        << "count is {" << count.first << ", " << count.second << "}\n";
      expect(that % ends_reached == 0);
      expect(that % 0 < ctx.memory_used());
    }  // destroy future

    expect(count == std::make_pair<int, int>(3, 3))
      << "count is {" << count.first << ", " << count.second << "}\n";
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };

  "Context Cancellation"_test = []() {
    // Setup
    test_context ctx;

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    auto a = [get_counter,
              &ends_reached](async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      std::println("[context cancel] a exited");
      ends_reached++;
      co_return;
    };
    auto b = [a, get_counter, &ends_reached](
               async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("[context cancel] b exited");
      ends_reached++;
      co_return;
    };
    auto c = [b, get_counter, &ends_reached](
               async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("[context cancel] c exited");
      ends_reached++;
      co_return;
    };

    expect(count == std::make_pair<int, int>(0, 0));
    expect(that % ends_reached == 0);

    auto future = c(ctx);

    expect(count == std::make_pair<int, int>(0, 0));
    expect(that % ends_reached == 0);

    future.resume();

    expect(count == std::make_pair<int, int>(3, 0));
    expect(that % ends_reached == 0);
    expect(that % 0 < ctx.memory_used());
    expect(that % false == future.has_value());
    expect(that % false == future.done());

    ctx.unsafe_cancel();

    expect(count == std::make_pair<int, int>(3, 3));
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
    expect(that % false == future.has_value());
    // Unfortunately, context doesn't have the information necessary to this
    // future. The future is invalid, but we currently cannot change its state
    // from the perview of the context.
    expect(that % false == future.done());
  };

  "Exception Propagation"_test = []() {
    // Setup
    test_context ctx;

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    bool should_throw = true;
    int step = 0;
    auto a = [&](async::context& p_ctx) -> async::future<void> {
      step = 3;
      std::println("[exception] entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      step = 4;
      if (should_throw) {
        std::println("[exception] ‼️ throwing runtime error");
        throw std::runtime_error("Throwing this error for the test");
      }
      std::println("[exception] a exited");
      ends_reached++;
      co_return;
    };
    auto b = [&](async::context& p_ctx) -> async::future<void> {
      step = 2;
      std::println("[exception] entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("[exception] b exited");
      ends_reached++;
      co_return;
    };
    auto c = [&](async::context& p_ctx) -> async::future<void> {
      step = 1;
      std::println("[exception] entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("[exception] c exited");
      ends_reached++;
      co_return;
    };

    expect(count == std::make_pair<int, int>(0, 0));
    expect(that % ends_reached == 0);

    auto future = c(ctx);

    expect(count == std::make_pair<int, int>(0, 0));
    expect(that % ends_reached == 0);

    std::println("Resume until future reaches suspension @ coroutine A");
    future.resume();

    expect(throws<std::runtime_error>([&]() {
      future.resume();
      future.resume();
    }))
      << "runtime_error exception was not thrown!";
    expect(that % future.done());
    expect(that % not future.has_value());
    expect(count == std::make_pair<int, int>(3, 3))
      << "count is {" << count.first << ", " << count.second << "}\n";
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };
};
