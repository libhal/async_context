#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void blocking_states()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  "co_await 10ms & co_await 50ms"_test = []() {
    // Setup
    async::inplace_context<1024> ctx;

    std::println("ctx.capacity() = {}", ctx.capacity());

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
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % 10ms == ctx.sleep_time());
    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3
    ctx.unblock();
    future.resume();

    // Verify 3
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 2 == step);
    expect(that % 25ms == ctx.sleep_time());

    // Exercise 4
    ctx.unblock();
    future.resume();

    // Verify 4
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 3 == step);
    expect(that % expected_return_value == future.value());
  };

  "context::block_by_io() "_test = []() {
    // Setup
    async::inplace_context<1024> ctx;

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
    expect(that % not future.done());
    expect(that % 0 == step);

    // Exercise 2: enter loop and block by io
    future.resume();

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % async::blocked_by::io == ctx.state());
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 3: stay in loop and re-block on io
    ctx.unblock();
    future.resume();

    // Verify 3
    expect(that % 0 < ctx.memory_used());
    expect(that % async::blocked_by::io == ctx.state());
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 4: unblock IO and resume to final suspend
    io_complete = true;
    ctx.unblock();
    future.resume();

    // Verify 4
    expect(that % 0 == ctx.memory_used());
    expect(that % async::blocked_by::nothing == ctx.state());
    expect(that % future.done());
    expect(that % 2 == step);
  };

  "blocked_by time, io, & sync"_test = []() {
    // Setup
    async::inplace_context<1024> ctx1{};
    async::inplace_context<1024> ctx2{};

    int step = 0;

    auto co = [&](async::context& p_context) -> async::future<void> {
      using namespace std::chrono_literals;
      step = 1;
      co_await 100us;
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
    expect(that % not future.done());
    expect(that % 0 == step);

    // Exercise 2
    future.resume();

    // Verify 2
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % 100us == ctx1.sleep_time());
    expect(that % ctx1.state() == async::blocked_by::time);
    expect(that % not future.done());
    expect(that % 1 == step);

    // Exercise 3
    ctx1.unblock();
    future.resume();

    // Verify 3
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % ctx1.state() == async::blocked_by::io)
      << "context should be blocked by IO";
    expect(that % not future.done());
    expect(that % 2 == step);

    // Exercise 4: move to blocked by sync
    ctx1.unblock();
    future.resume();

    // Verify 4
    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
    expect(that % not future.done());
    expect(that % &ctx2 == ctx1.get_blocker())
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

int main()
{
  blocking_states();
}
