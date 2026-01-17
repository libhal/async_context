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

    counter_pair count{};
    int ends_reached = 0;

    auto a = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering a");
      raii_counter counter(count, "a");
      co_await std::suspend_always{};
      std::println("[future cancel] a exited");
      ends_reached++;
      co_return;
    };

    auto b = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering b");
      raii_counter counter(count, "b");
      co_await a(p_ctx);
      std::println("[future cancel] b exited");
      ends_reached++;
      co_return;
    };

    auto c = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel] entering c");
      raii_counter counter(count, "c");
      co_await b(p_ctx);
      std::println("[future cancel] c exited");
      ends_reached++;
      co_return;
    };

    {
      expect(that % count == counter_pair{ 0, 0 });
      expect(that % ends_reached == 0);

      auto future = c(ctx);

      expect(that % count == counter_pair{ 0, 0 });
      expect(that % ends_reached == 0);

      future.resume();

      expect(that % count == counter_pair{ 3, 0 });
      expect(that % ends_reached == 0);
      expect(that % 0 < ctx.memory_used());
    }  // destroy future

    expect(that % count == counter_pair{ 3, 3 });
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };

  "Cancellation"_test = []() {
    // Setup
    test_context ctx;

    counter_pair count{};
    int ends_reached = 0;

    auto a = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel direct] entering a");
      raii_counter counter(count, "future direct A");
      std::println("[context cancel] suspend a");
      co_await std::suspend_always{};
      std::println("[future cancel direct] a exited");
      ends_reached++;
      co_return;
    };

    auto b = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel direct] entering b");
      raii_counter counter(count, "future direct B");
      co_await a(p_ctx);
      std::println("[context cancel] suspend b");  // should never show up
      co_await std::suspend_always{};
      std::println("[future cancel direct] b exited");
      ends_reached++;
      co_return;
    };

    auto c = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[future cancel direct] entering c");
      raii_counter counter(count, "future direct C");
      co_await b(p_ctx);
      std::println("[context cancel] suspend c");  // should never show up
      co_await std::suspend_always{};
      std::println("[future cancel direct] c exited");
      ends_reached++;
      co_return;
    };

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    auto future = c(ctx);

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    future.resume();

    expect(that % count == counter_pair{ 3, 0 });
    expect(that % ends_reached == 0);
    expect(that % 0 < ctx.memory_used());

    future.cancel();

    expect(that % count == counter_pair{ 3, 3 });
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };

  "Context Cancellation"_test = []() {
    // Setup
    test_context ctx;

    counter_pair count{};
    int ends_reached = 0;

    auto a = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering a");
      raii_counter counter(count, "context A");
      std::println("[context cancel] suspend a");
      co_await std::suspend_always{};
      std::println("[context cancel] a exited");
      ends_reached++;
      co_return;
    };

    auto b = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering b");
      raii_counter counter(count, "context B");
      co_await a(p_ctx);
      std::println("[context cancel] suspend b");  // should never show up
      co_await std::suspend_always{};
      std::println("[context cancel] b exited");
      ends_reached++;
      co_return;
    };

    auto c = [&](async::context& p_ctx) -> async::future<void> {
      std::println("[context cancel] entering c");
      raii_counter counter(count, "context C");
      co_await b(p_ctx);
      std::println("[context cancel] c suspended");  // should never show up
      co_await std::suspend_always{};
      std::println("[context cancel] c exited");
      ends_reached++;
      co_return;
    };

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    auto future = c(ctx);

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    future.resume();

    expect(that % count == counter_pair{ 3, 0 });
    expect(that % ends_reached == 0);
    expect(that % 0 < ctx.memory_used());
    expect(that % false == future.has_value());
    expect(that % false == future.done());

    ctx.cancel();

    expect(that % count == counter_pair{ 3, 3 });
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
    expect(that % false == future.has_value());
  };

  "Exception Propagation"_test = []() {
    // Setup
    test_context ctx;

    counter_pair count{};
    int ends_reached = 0;

    bool should_throw = true;
    int step = 0;
    auto a = [&](async::context& p_ctx) -> async::future<void> {
      step = 3;
      std::println("[exception] entering a");
      raii_counter counter(count, "exception A");
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
      raii_counter counter(count, "exception B");
      co_await a(p_ctx);
      std::println("[exception] b exited");
      ends_reached++;
      co_return;
    };

    auto c = [&](async::context& p_ctx) -> async::future<void> {
      step = 1;
      std::println("[exception] entering c");
      raii_counter counter(count, "exception C");
      co_await b(p_ctx);
      std::println("[exception] c exited");
      ends_reached++;
      co_return;
    };

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    auto future = c(ctx);

    expect(that % count == counter_pair{ 0, 0 });
    expect(that % ends_reached == 0);

    std::println("Resume until future reaches suspension @ coroutine A");
    future.resume();

    expect(throws<std::runtime_error>([&]() { future.resume(); }))
      << "runtime_error exception was not thrown!";
    expect(that % future.done());
    expect(that % not future.has_value());
    expect(that % count == counter_pair{ 3, 3 });
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };
};
