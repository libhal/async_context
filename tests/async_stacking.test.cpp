#include <coroutine>

#include <boost/ut.hpp>

import async_context;
import test_utils;

// doesn't matter...
// bar runs until completion then foo runs until complete
void async_stacking()
{
  using namespace boost::ut;

  // LIFO stacking: the last routine pushed onto the context is the first
  // to execute. When two routines are loaded onto the same context, the
  // second one loaded runs first and the first one loaded runs last.

  "two routines lifo order two steps each"_test = []() {
    // Setup
    async::inplace_context<1024> ctx;

    unsigned step = 0;

    auto routine_a = [&step](async::context&) -> async::future<void> {
      // routine_a is loaded first; it runs last (LIFO)
      step = 3;
      co_await std::suspend_always{};
      step = 4;
      co_await std::suspend_always{};
      co_return;
    };

    auto routine_b = [&step](async::context&) -> async::future<void> {
      // routine_b is loaded second; it runs first (LIFO)
      step = 1;
      co_await std::suspend_always{};
      step = 2;
      co_await std::suspend_always{};
      co_return;
    };

    // Exercise: load routine_a first, then routine_b
    auto future_a = routine_a(ctx);
    auto future_b = routine_b(ctx);

    // Verify: neither has started yet
    expect(that % not future_a.done());
    expect(that % not future_b.done());
    expect(that % 0 == step);

    // Exercise: first resume — routine_b (last loaded) runs first, hits step=1
    // then suspends
    future_b.resume();

    // Verify: routine_b ran first and suspended at step 1
    expect(that % not future_b.done());
    expect(that % not future_a.done());
    expect(that % 1 == step);

    // Exercise: second resume — routine_b resumes, hits step=2, suspends again
    future_b.resume();

    // Verify: routine_b suspended at step 2; routine_a still waiting
    expect(that % not future_b.done());
    expect(that % not future_a.done());
    expect(that % 2 == step);

    // Exercise: third resume — routine_b hits co_return and completes
    future_b.resume();

    // Verify: routine_b is done; routine_a still waiting
    expect(that % future_b.done());
    expect(that % not future_a.done());
    expect(that % 3 == step);

    // Exercise: fourth resume — routine_a (first loaded) now runs, hits step=3,
    // then suspends
    future_a.resume();

    // Verify: routine_a ran and suspended at step 3
    expect(that % not future_a.done());
    expect(that % 4 == step);

    // Exercise: fifth resume — routine_a resumes, hits step=4, suspends again
    future_a.resume();

    // Verify: routine_a is done; all memory released
    expect(that % future_a.done());
    expect(that % 0 == ctx.memory_used());
    expect(that % 4 == step);
  };

  "three routines lifo order"_test = []() {
    // Setup
    async::inplace_context<2048> ctx;

    unsigned step = 0;

    auto routine_a = [&step](async::context&) -> async::future<void> {
      // loaded first — runs last
      step = 7;
      co_await std::suspend_always{};
      step = 8;
      co_await std::suspend_always{};
      co_return;
    };

    auto routine_b = [&step](async::context&) -> async::future<void> {
      // loaded second — runs second
      step = 4;
      co_await std::suspend_always{};
      step = 5;
      co_await std::suspend_always{};
      co_return;
    };

    auto routine_c = [&step](async::context&) -> async::future<void> {
      // loaded third — runs first (LIFO)
      step = 1;
      co_await std::suspend_always{};
      step = 2;
      co_await std::suspend_always{};
      co_return;
    };

    // Load in order: a, b, c
    auto future_a = routine_a(ctx);
    auto future_b = routine_b(ctx);
    auto future_c = routine_c(ctx);

    expect(that % 0 == step);

    // routine_c runs first
    future_c.resume();
    expect(that % 1 == step);
    expect(that % not future_c.done());

    future_c.resume();
    expect(that % 2 == step);
    expect(that % not future_c.done());

    future_c.resume();
    expect(that % 4 == step);
    expect(that % future_c.done());

    // routine_b was already started by routine_c's co_return; resume to step=5
    future_b.resume();
    expect(that % 5 == step);
    expect(that % not future_b.done());

    // routine_b completes and immediately starts routine_a which runs to step=7
    future_b.resume();
    expect(that % 7 == step);
    expect(that % future_b.done());
    expect(that % not future_a.done());

    future_a.resume();
    expect(that % 8 == step);
    expect(that % not future_a.done());

    future_a.resume();
    expect(that % 8 == step);
    expect(that % future_a.done());

    // All memory should be released
    expect(that % 0 == ctx.memory_used());
  };
}

int main()
{
  async_stacking();
}
