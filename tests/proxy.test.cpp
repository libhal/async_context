#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

boost::ut::suite<"proxy_tests"> proxy_tests = []() {
  using namespace boost::ut;
  using namespace std::chrono_literals;

  "Proxy Context (normal behavior, no timeout)"_test = []() {
    // Setup
    test_context ctx;
    std::println("====================================");
    std::println("Running Proxy Context Test (no timeout normal behavior)");
    std::println("====================================");

    static constexpr auto expected_suspensions = 5;
    static constexpr auto timeout_count = expected_suspensions + 2;
    auto suspension_count = 0;

    auto b = [&suspension_count](async::context&) -> async::future<int> {
      while (suspension_count < expected_suspensions) {
        suspension_count++;
        // TODO(#44): For some reason this segfaults on Linux
        // std::println("p_suspend_count = {}!", suspension_count);
        co_await std::suspend_always{};
      }
      co_return expected_suspensions;
    };

    auto a = [b](async::context& p_ctx) -> async::future<int> {
      std::println("Entered coroutine a!");
      auto proxy = async::proxy_context::from(p_ctx);
      std::println("Made a proxy!");
      int counter = timeout_count;
      auto supervised_future = b(proxy);

      while (not supervised_future.done()) {
        std::println("supervised_future not done()!");
        if (counter <= 0) {
          std::println("TIMEDOUT detected!");
          break;
        }
        std::println("resuming supervised_future...");
        supervised_future.resume();

        std::println("suspending ourself...");
        co_await std::suspend_always{};
        counter--;
      }

      std::println("finished while loop()!");

      if (counter > 0) {
        if (supervised_future.has_value()) {
          std::println("✅ SUCCESS!");
          co_return supervised_future.value();
        } else {
          std::println("‼️ No value after completion!");
          co_return -2;
        }
      } else {
        std::println("‼️ TIMED OUT!!");
        co_return -1;
      }

      std::println("TIMED OUT!!");

      co_return -1;
    };

    auto my_future = a(ctx);
    while (not my_future.done()) {
      my_future.resume();
    }

    expect(my_future.has_value());
    auto value = my_future.value();

    expect(that % my_future.done());
    expect(that % expected_suspensions == value);
    expect(that % 0 == ctx.memory_used());
    expect(that % suspension_count == expected_suspensions);
  };

  "Proxy Coroutines Timeout"_test = []() {
    // Setup
    test_context ctx;
    std::println("====================================");
    std::println("Running Proxy Context Test (with timeout)");
    std::println("====================================");

    static constexpr auto expected_suspensions = 5;
    static constexpr auto timeout_count = expected_suspensions - 2;
    auto suspension_count = 0;

    auto b = [&suspension_count](async::context&) -> async::future<int> {
      suspension_count = 0;
      while (suspension_count < expected_suspensions) {
        suspension_count++;
        // TODO(#44): For some reason this segfaults on Linux
        // std::println("p_suspend_count = {}!", suspension_count);
        co_await std::suspend_always{};
      }
      co_return expected_suspensions;
    };

    auto a = [b](async::context& p_ctx) -> async::future<int> {
      std::println("Entered coroutine a!");
      auto proxy = async::proxy_context::from(p_ctx);
      std::println("Made a proxy!");
      int counter = timeout_count;
      auto supervised_future = b(proxy);

      while (not supervised_future.done()) {
        std::println("supervised_future not done()!");
        if (counter <= 0) {
          std::println("TIMEDOUT detected!");
          break;
        }
        std::println("resuming supervised_future...");
        supervised_future.resume();

        std::println("suspending ourself...");
        co_await std::suspend_always{};
        counter--;
      }

      std::println("finished while loop()!");

      if (counter > 0) {
        if (supervised_future.has_value()) {
          std::println("✅ SUCCESS!");
          co_return supervised_future.value();
        } else {
          std::println("‼️ No value after completion!");
          co_return -2;
        }
      } else {
        std::println("‼️ TIMED OUT!!");
        co_return -1;
      }
    };

    auto future = a(ctx);

    while (not future.done()) {
      future.resume();
    }
    auto value = future.value();

    expect(that % future.done());
    expect(that % -1 == value);
    expect(that % suspension_count == timeout_count);
    expect(that % 0 == ctx.memory_used());
  };
};
