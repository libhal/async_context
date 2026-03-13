#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void on_upload_test()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  "on_upload() via lambda"_test = []() {
    // Setup
    async::inplace_context<1024> ctx;
    bool unblock_called = false;
    async::context const* unblocked_context = nullptr;
    auto upload_handler =
      async::unblock_listener::from([&](async::context const& p_context) {
        unblock_called = true;
        unblocked_context = &p_context;
      });
    ctx.on_unblock(&upload_handler);

    unsigned step = 0;
    auto co = [&step](async::context&) -> async::future<void> {
      step = 1;
      co_await 10ms;
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
    expect(that % unblock_called == false);
    expect(that % unblocked_context == nullptr);
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % 10ms == ctx.sleep_time());
    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3
    ctx.unblock();
    future.resume();

    // Verify 3
    expect(that % unblock_called == true);
    expect(that % unblocked_context == &ctx);
    expect(that % unblock_called == true);
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % 1 == step);

    ctx.clear_unblock_listener();
  };

  "on_upload() via inheritance"_test = []() {
    // Setup
    async::inplace_context<1024> ctx;
    struct un_blocker : public async::unblock_listener
    {
      bool unblock_called = false;
      async::context const* unblocked_context = nullptr;

      void on_unblock(async::context const& p_context) noexcept override
      {
        unblock_called = true;
        unblocked_context = &p_context;
      }
    };

    un_blocker ub;
    ctx.on_unblock(&ub);

    unsigned step = 0;
    auto co = [&step](async::context&) -> async::future<void> {
      step = 1;
      co_await 10ms;
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
    expect(that % ub.unblock_called == false);
    expect(that % ub.unblocked_context == nullptr);
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % 10ms == ctx.sleep_time());
    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3
    ctx.unblock();
    future.resume();

    // Verify 3
    expect(that % ub.unblock_called == true);
    expect(that % ub.unblocked_context == &ctx);
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % 1 == step);

    ctx.clear_unblock_listener();
  };
};

int main()
{
  on_upload_test();
}
