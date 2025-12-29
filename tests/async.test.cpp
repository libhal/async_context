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
#include <memory_resource>
#include <ostream>
#include <source_location>
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

async::future<void> coro_print(async::context&)
{
  using namespace std::chrono_literals;
  std::println("Printed from a coroutine");
  co_await 100ns;
  resumption_occurred = true;
  co_await 100ns;
  co_return;
}

namespace async {
void async_context_suite()
{
  using namespace boost::ut;

  "coroutine with time-based blocking and sync_wait"_test = []() {
    // Setup
    auto scheduler =
      mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
    async::context my_context(scheduler, 1024);

    // Exercise
    auto future_print = coro_print(my_context);
    future_print.sync_wait();

    // Verify
    expect(that % resumption_occurred);
    expect(that % future_print.done());
    expect(that % 2 == scheduler->sleep_count);
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
        expect(that % my_context1.state() ==
               access_first.handle().promise().get_context().state())
          << "line: " << p_location.line() << '\n';
        expect(that % static_cast<int>(p_state) ==
               static_cast<int>(my_context1.state()))
          << "line: " << p_location.line() << '\n';
        ;
      };

    auto check_access_second_blocked_by =
      [&](async::blocked_by p_state = async::blocked_by::nothing,
          std::source_location const& p_location =
            std::source_location::current()) {
        expect(that % my_context2.state() ==
               access_second.handle().promise().get_context().state())
          << "line: " << p_location.line() << '\n';
        ;
        expect(that % p_state == my_context2.state())
          << "line: " << p_location.line() << '\n';
        ;
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
};
}  // namespace async
