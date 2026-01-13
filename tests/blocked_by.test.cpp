#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

boost::ut::suite<"blocking_states"> blocking_states = []() {
  using namespace boost::ut;
  using namespace std::chrono_literals;

  "co_await 10ms & co_await 50ms"_test = []() {
    // Setup
    test_context ctx;

    static constexpr int expected_return_value = 8748;
    unsigned step = 0;
    auto co = [&step](async::context&) -> async::future<int> {
      step = 1;
      co_await 10ms;
      step = 2;
      co_await 25ms;
      step = 3;
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

    // Exercise 2
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % ctx.info->scheduled_called_once);
    expect(that % 1 == ctx.info->sleep_count);
    expect(that % not future.done());
    expect(that % 10ms == ctx.info->last_sleep_time);

    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3
    future.resume();

    // Verify 3
    expect(that % 0 < ctx.memory_used());
    expect(that % 2 == ctx.info->sleep_count);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 2 == step);
    expect(that % 25ms == ctx.info->last_sleep_time);

    // Exercise 4
    future.resume();

    // Verify 4
    expect(that % 0 == ctx.memory_used());
    expect(that % 2 == ctx.info->sleep_count);
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 3 == step);
    expect(that % expected_return_value == future.value());
  };

  "context::block_by_io() "_test = []() {
    // Setup
    test_context ctx;

    unsigned step = 0;
    bool io_complete = false;

    auto co = [&step,
               &io_complete](async::context& p_ctx) -> async::future<void> {
      step = 1;
      io_complete = false;

      while (not io_complete) {
        co_await p_ctx.block_by_io();
      }

      step = 2;

      co_return;
    };

    // Exercise 1
    auto future = co(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not ctx.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % 0 == step);

    // Exercise 2: enter loop and block by io
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % ctx.info->scheduled_called_once);
    expect(that % ctx.info->io_block);
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 3: stay in loop and re-block on io
    future.resume();

    // Verify 3
    expect(that % 0 < ctx.memory_used());
    expect(that % ctx.info->scheduled_called_once);
    expect(that % ctx.info->io_block);
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 4: unblock IO and resume to final suspend
    io_complete = true;
    future.resume();

    // Verify 4
    expect(that % 0 == ctx.memory_used());
    expect(that % ctx.info->scheduled_called_once);
    expect(that % ctx.info->io_block);
    expect(that % future.done());
    expect(that % 2 == step);
  };

  "blocked_by time, io, & sync"_test = []() {
    // Setup
    auto info = std::make_shared<thread_info>();
    test_context ctx1(info);
    test_context ctx2(info);

    int step = 0;

    auto co = [&](async::context& p_context) -> async::future<void> {
      using namespace std::chrono_literals;
      step = 1;
      co_await 100ns;
      step = 2;
      co_await p_context.block_by_io();
      step = 3;
      co_await p_context.block_by_sync(&ctx2);
      step = 4;
      co_return;
    };

    // Exercise 1
    auto future = co(ctx1);

    // Verify 1
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % not ctx1.info->scheduled_called_once);
    expect(that % not future.done());
    expect(that % 0 == step);

    // Exercise 2
    future.resume();

    // Verify 2
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % ctx1.info->scheduled_called_once);
    expect(that % 100ns == ctx1.info->last_sleep_time);
    expect(that % not ctx1.info->io_block);
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 3
    future.resume();

    // Verify 3
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % ctx1.info->scheduled_called_once);
    expect(that % ctx1.info->io_block) << "context should be blocked by IO";
    expect(that % not future.done());
    expect(that % 2 == step);

    // Exercise 4: move to blocked by sync
    ctx1.unblock();
    future.resume();

    // Verify 4
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % not future.done());
    expect(that % &ctx2 == ctx1.info->sync_context)
      << "sync context should be &ctx2";
    expect(that % 3 == step);

    // Exercise 5: finish
    future.resume();

    // Verify 5
    expect(that % 0 == ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % future.done());
    expect(that % 4 == step);
  };
};
