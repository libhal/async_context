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

#include <chrono>
#include <coroutine>
#include <functional>
#include <memory_resource>
#include <ostream>
#include <source_location>
#include <stdexcept>
#include <variant>

#include <boost/ut.hpp>

import async_context;

namespace async {
std::ostream& operator<<(std::ostream& out, blocked_by b)
{
  switch (b) {
    case blocked_by::nothing:
      return out << "nothing";
    case blocked_by::time:
      return out << "time";
    case blocked_by::io:
      return out << "io";
    case blocked_by::sync:
      return out << "sync";
    case blocked_by::external:
      return out << "external";
    default:
      // For unknown values we print the numeric value
      return out << "blocked_by(" << static_cast<std::uint8_t>(b) << ')';
  }
}
}  // namespace async

bool resumption_occurred = false;
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
    std::println("Scheduler called!", sleep_count);

    switch (p_block_state) {
      case async::blocked_by::time: {
        if (std::holds_alternative<std::chrono::nanoseconds>(p_block_info)) {
          std::println("sleep for: {}",
                       std::get<std::chrono::nanoseconds>(p_block_info));
          sleep_count++;
          std::println("Sleep count = {}!", sleep_count);
        }
        break;
      }
      case async::blocked_by::sync: {
        if (std::holds_alternative<async::context*>(p_block_info)) {
          auto* context = std::get<async::context*>(p_block_info);
          std::println(
            "Coroutine ({}) is blocked by syncronization with coroutine ({})",
            static_cast<void*>(&p_context),
            static_cast<void*>(context));
          sync_context = context;
        }
        break;
      }
      case async::blocked_by::io: {
        io_block = true;
        break;
      }
      case async::blocked_by::nothing: {
        std::println("Context ({}) has been unblocked!",
                     static_cast<void*>(&p_context));
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

namespace async {
void async_context_suite()
{
  using namespace boost::ut;

  "coroutine with time-based blocking and sync_wait"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context(scheduler, 1024);

    static constexpr int expected_return_value = 5;

    auto print_and_wait_coroutine = [](async::context&) -> async::future<int> {
      using namespace std::chrono_literals;
      std::println("Printed from a coroutine");
      co_await 100ns;
      resumption_occurred = true;
      co_await 100ns;
      co_return expected_return_value;
    };

    // Exercise
    auto future_print = print_and_wait_coroutine(my_context);
    auto value = future_print.sync_wait();

    // Verify
    expect(that % resumption_occurred);
    expect(that % future_print.done());
    expect(that % 2 == scheduler->sleep_count);
    expect(that % expected_return_value == value);
  };

  "block_by_io and block_by_sync notify scheduler correctly"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context(scheduler, 1024);
    async::context my_context2(scheduler, 1024);

    resumption_occurred = false;

    auto test_coro =
      [&my_context2](async::context& p_context) -> async::future<void> {
      using namespace std::chrono_literals;
      std::println("Printed from a coroutine");
      co_await 100ns;
      resumption_occurred = true;
      co_await p_context.block_by_io();
      co_await p_context.block_by_sync(&my_context2);
      co_return;
    };

    // Exercise
    auto blocked_by_testing = test_coro(my_context);
    expect(that % not resumption_occurred);
    blocked_by_testing.sync_wait();

    // Verify
    expect(that % resumption_occurred);
    expect(that % blocked_by_testing.done());
    expect(that % scheduler->io_block);
    expect(that % &my_context2 == scheduler->sync_context);
    expect(that % 0 == my_context.memory_used());
    expect(that % 0 == my_context2.memory_used());
  };

  "mutex-like resource synchronization between coroutines"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context1(scheduler, 1024);
    async::context my_context2(scheduler, 1024);

    async::context_token io_in_use;

    auto single_resource =
      [&](async::context& p_context) -> async::future<void> {
      using namespace std::chrono_literals;

      std::println("Executing 'single_resource' coroutine");
      while (io_in_use) {
        std::println("Resource unavailable, blocked by {}",
                     io_in_use.address());
        co_await io_in_use.set_as_block_by_sync(p_context);
      }

      // Block next coroutine from using this resource
      io_in_use = p_context;

      // setup dma transaction...

      // It cannot be assumed that the scheduler will not sync_wait() this
      // coroutine, thus, a loop is required to sure that the async operation
      // has actually completed.
      while (io_in_use == p_context) {
        std::println("Waiting on io complete flag, blocking by I/O");
        // Continually notify that this is blocked by IO
        co_await p_context.block_by_io();
      }

      std::println("IO operation complete! Returning!");

      co_return;
    };

    std::println("🧱 Future setup");
    auto access_first = single_resource(my_context1);
    auto access_second = single_resource(my_context2);

    auto check_access_first_blocked_by =
      [&](async::blocked_by p_state = async::blocked_by::io,
          std::source_location const& p_location =
            std::source_location::current()) {
        expect(that % static_cast<int>(p_state) ==
               static_cast<int>(my_context1.state()))
          << "line: " << p_location.line() << '\n';
      };

    auto check_access_second_blocked_by =
      [&](async::blocked_by p_state = async::blocked_by::nothing,
          std::source_location const& p_location =
            std::source_location::current()) {
        expect(that % p_state == my_context2.state())
          << "line: " << p_location.line() << '\n';
      };

    // access_first will claim the resource and will return control, and be
    // blocked by IO.
    std::println("▶️ Resume 1st: 1");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 1st: 2");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 1st: 3");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 2nd: 1");
    access_second.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by(async::blocked_by::sync);

    io_in_use.unblock_and_clear();

    check_access_first_blocked_by(async::blocked_by::nothing);
    check_access_second_blocked_by(async::blocked_by::sync);

    std::println("▶️ Resume 2nd: 2");
    access_second.resume();

    // Resuming access_second shouldn't change the state of anything
    check_access_first_blocked_by(async::blocked_by::nothing);
    check_access_second_blocked_by(async::blocked_by::io);

    std::println("▶️ Resume 1st: 4, this should finish the operation");
    access_first.resume();

    expect(that % my_context1.state() == async::blocked_by::nothing);
    expect(that % access_first.done());

    check_access_second_blocked_by(async::blocked_by::io);
    access_second.resume();
    check_access_second_blocked_by(async::blocked_by::io);

    io_in_use.unblock_and_clear();
    access_second.resume();

    expect(that % my_context2.state() == async::blocked_by::nothing);
    expect(that % access_second.done());
  };

  skip / "cancellations tests"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context ctx(scheduler, 1024);

    std::println("====================================");
    std::println("Running cancellation test");
    std::println("====================================");

    struct raii_counter
    {
      raii_counter(std::pair<int*, int*> p_counts)
        : counts(p_counts)
      {
        std::println("🔨 Constructing...");
        (*counts.first)++;
      }

      ~raii_counter()  // NOLINT(bugprone-exception-escape)
      {
        std::println("💥 Destructing...");
        (*counts.second)++;
      }
      std::pair<int*, int*> counts;
    };

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    auto a = [get_counter,
              &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      std::println("a exited");
      ends_reached++;
      co_return;
    };
    auto b =
      [a, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("b exited");
      ends_reached++;
      co_return;
    };
    auto c =
      [b, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("c exited");
      ends_reached++;
      co_return;
    };

    {
      expect(count == std::make_pair<int, int>(0, 0));
      expect(that % ends_reached == 0);

      auto future = c(ctx);

      expect(count == std::make_pair<int, int>(0, 0));
      expect(that % ends_reached == 0);

      std::println("Resume until future reaches suspension @ coroutine A");
      future.resume();

      expect(count == std::make_pair<int, int>(3, 0));
      expect(that % ends_reached == 0);
      expect(that % 0 < ctx.memory_used());
    }  // destroy future

    expect(count == std::make_pair<int, int>(3, 3))
      << "count is {" << count.first << ", " << count.second << "}\n";
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());

    std::println(">>>>>>>>>>>>>>>>>>>>>>>>>>>");
  };

  skip / "Context Cancellation"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context ctx(scheduler, 1024);

    std::println("====================================");
    std::println("Running Context Cancellation");
    std::println("====================================");

    struct raii_counter
    {
      raii_counter(std::pair<int*, int*> p_counts)
        : counts(p_counts)
      {
        std::println("🔨 Constructing...");
        (*counts.first)++;
      }

      ~raii_counter()  // NOLINT(bugprone-exception-escape)
      {
        std::println("💥 Destructing...");
        (*counts.second)++;
      }
      std::pair<int*, int*> counts;
    };

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    auto a = [get_counter,
              &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      std::println("a exited");
      ends_reached++;
      co_return;
    };
    auto b =
      [a, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("b exited");
      ends_reached++;
      co_return;
    };
    auto c =
      [b, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("c exited");
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

    std::println(">>>>>>>>>>>>>>>>>>>>>>>>>>>");
  };

  skip / "Exception Propagation"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context ctx(scheduler, 1024);

    std::println("====================================");
    std::println("Running Exception Propagation Test");
    std::println("====================================");

    struct raii_counter
    {
      raii_counter(std::pair<int*, int*> p_counts)
        : counts(p_counts)
      {
        std::println("🔨 Constructing...");
        (*counts.first)++;
      }

      ~raii_counter()  // NOLINT(bugprone-exception-escape)
      {
        std::println("💥 Destructing...");
        (*counts.second)++;
      }
      std::pair<int*, int*> counts;
    };

    std::pair<int, int> count{ 0, 0 };
    int ends_reached = 0;

    auto get_counter = [&count]() -> auto {
      return raii_counter(
        std::make_pair<int*, int*>(&count.first, &count.second));
    };

    bool should_throw = true;
    auto a = [get_counter, &should_throw, &ends_reached](
               async::context& p_ctx) -> future<void> {
      std::println("entering a");
      raii_counter counter = get_counter();
      co_await std::suspend_always{};
      if (should_throw) {
        throw std::runtime_error("Throwing this error for the test");
      }
      std::println("a exited");
      ends_reached++;
      co_return;
    };
    auto b =
      [a, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering b");
      raii_counter counter = get_counter();
      co_await a(p_ctx);
      std::println("b exited");
      ends_reached++;
      co_return;
    };
    auto c =
      [b, get_counter, &ends_reached](async::context& p_ctx) -> future<void> {
      std::println("entering c");
      raii_counter counter = get_counter();
      co_await b(p_ctx);
      std::println("c exited");
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

    expect(throws<std::runtime_error>([&future]() {
      std::println("This resume should throw an runtime_error");
      future.sync_wait();
    }))
      << "runtime_error Exception was not caught!";
    expect(that % true == future.done());
    expect(that % false == future.has_value());
    expect(count == std::make_pair<int, int>(3, 3))
      << "count is {" << count.first << ", " << count.second << "}\n";
    expect(that % ends_reached == 0);
    expect(that % 0 == ctx.memory_used());
  };

  skip / "Proxy Context (no timeout normal behavior)"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context1(scheduler, 1024);
    std::println("====================================");
    std::println("Running Proxy Context Test (no timeout normal behavior)");
    std::println("====================================");

    static constexpr auto expected_suspensions = 5;

    auto b = [](async::context&, int p_suspend_count) -> future<int> {
      auto const result = p_suspend_count;
      while (p_suspend_count > 0) {
        p_suspend_count--;
        std::println("p_suspend_count = {}!", p_suspend_count);
        co_await std::suspend_always{};
      }
      co_return result;
    };

    auto a = [b](async::context& p_ctx) -> future<int> {
      std::println("Entered coroutine a!");
      auto proxy = p_ctx.borrow_proxy();
      std::println("Made a proxy!");
      int counter = expected_suspensions + 2;
      auto supervised_future = b(proxy, expected_suspensions);

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
        std::println("✅ SUCCESS!");
        co_return supervised_future.sync_wait();
      }

      std::println("TIMED OUT!!");

      co_return -1;
    };

    auto my_future = a(my_context1);
    auto value = my_future.sync_wait();

    expect(that % my_future.done());
    expect(that % expected_suspensions == value);
  };

  skip / "Proxy Coroutines Timeout"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context1(scheduler, 1024);
    std::println("====================================");
    std::println("Running Proxy Context Test (with timeout)");
    std::println("====================================");

    static constexpr auto expected_suspensions = 5;

    [[maybe_unused]] auto b = [](async::context&,
                                 int p_suspend_count) -> future<int> {
      auto const result = p_suspend_count;
      while (p_suspend_count > 0) {
        p_suspend_count--;
        std::println("p_suspend_count = {}!", p_suspend_count);
        co_await std::suspend_always{};
      }
      co_return result;
    };

    auto a = [b](async::context& p_ctx) -> future<int> {
      std::println("Entered coroutine a!");
      auto proxy = p_ctx.borrow_proxy();
      std::println("Made a proxy!");
      int counter = expected_suspensions - 2;
      auto supervised_future = b(proxy, expected_suspensions);

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
        std::println("✅ SUCCESS!");
        co_return supervised_future.sync_wait();
      }

      std::println("‼️ TIMED OUT!!");

      co_return -1;
    };

    auto my_future = a(my_context1);
    auto value = my_future.sync_wait();

    expect(that % my_future.done());
    expect(that % -1 == value);
    expect(that % 0 == my_context1.memory_used());
  };
};
}  // namespace async
