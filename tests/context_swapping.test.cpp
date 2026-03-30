#include <coroutine>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void context_swapping()
{
  using namespace boost::ut;

  // Context swapping: one routine runs on an outer context and produces a
  // future. A second routine on a separate context co_awaits that future using
  // a locally nested context. Awaiting a future whose context was allocated
  // inside a coroutine frame is a contract violation and will abort.

  "co_await future from another context"_test = []() {
    // Setup
    async::inplace_context<1024> ctx;

    unsigned step = 0;

    // routine_a produces a future that routine will await
    auto routine_a = [&step](async::context& p_ctx) -> async::future<int> {
      step = 1;
      co_await std::suspend_always{};
      step = 2;
      co_await std::suspend_always{};
      step = 3;
      co_return 42;
    };

    // routine captures routine_a and co_awaits it using a nested context
    // allocated inside the coroutine frame — this is a contract violation
    auto routine = [&step, &routine_a](async::context&) -> async::future<int> {
      step = 10;
      co_await std::suspend_always{};

      async::inplace_context<128> nested_ctx;
      // This is a bug and a contract violation, this will either call the
      // contract violator OR call std::terminate
      auto result = co_await routine_a(nested_ctx);
      // These should never be reached
      step = 11;
      co_return result;
    };

    // Exercise: start routine on the outer context
    auto future = routine(ctx);

    // Verify: routine has not run yet
    expect(that % not future.done());
    expect(that % 0 == step);
    // ctx holds routine's frame
    expect(that % 0 < ctx.memory_used());

    // Start routine — it runs to step=10 then suspends at the first
    // co_await suspend_always
    future.resume();
    expect(that % 10 == step);
    expect(that % not future.done());

    // Resume routine — it allocates nested_ctx and starts routine_a,
    // which runs to step=1 then suspends
    future.resume();
    expect(that % 1 == step);

// Resume routine — it hits co_await on a future whose context was
// allocated inside the coroutine frame; this is a contract violation
#if defined(__cpp_contracts)
    // TODO(kammce): Add violation handler
#else
    // NOTE: aborts() forks a child process, so a std::set_terminate handler
    // set here cannot be observed from the parent — terminate_was_called would
    // always be false regardless. The aborts() check is the only observable
    // assertion available across the process boundary.
    expect(aborts([&future]() { future.resume(); }));
    expect(that % 1 == step);
    expect(that % not future.done());
    // ctx holds routine's suspended frame while the nested context does work
#endif
  };
}

int main()
{
  context_swapping();
}
