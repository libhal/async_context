#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void context_listener_test()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  // Setup
  struct listener : public async::context_listener
  {
    async::context const* unblocked_context = nullptr;
    async::context const* sync_blocker = nullptr;
    async::context* sync_blocked = nullptr;

    void on_unblock(async::context& p_context) noexcept override
    {
      unblocked_context = &p_context;
    }

    void on_sync_block(async::context& p_blocked,
                       async::context const& p_blocker) noexcept override
    {
      sync_blocker = &p_blocker;
      sync_blocked = &p_blocked;
    }

    void reset()
    {
      unblocked_context = nullptr;
      sync_blocker = nullptr;
      sync_blocked = nullptr;
    }
  };

  listener listener_obj;
  async::inplace_context<1024> ctx1;
  async::inplace_context<1024> ctx2;
  ctx1.set_listener(&listener_obj);
  ctx2.set_listener(&listener_obj);

  async::mutex mutex;

  auto coro = [&](async::context& p_context) -> async::future<void> {
    auto lock = co_await mutex.lock(p_context);
    co_await 1ms;
    co_return;
  };

  // Exercise 1
  auto future1 = coro(ctx1);
  auto future2 = coro(ctx2);

  // Verify 1
  expect(that % not future1.done());
  expect(that % not future2.done());
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());
  expect(that % nullptr == listener_obj.unblocked_context);
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  // Exercise 2
  future1.resume();  // should acquire resource and get blocked by time.
  future2.resume();  // should block by sync

  // Verify 2
  expect(that % async::blocked_by::time == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % 1ms == ctx1.sleep_time());
  expect(that % &ctx1 == mutex.owner());
  expect(that % nullptr == listener_obj.unblocked_context);
  expect(that % &ctx2 == listener_obj.sync_blocked);
  expect(that % &ctx1 == listener_obj.sync_blocker);

  // Exercise 3
  listener_obj.reset();
  ctx1.unblock();

  // Verify 3
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % &ctx1 == mutex.owner());
  expect(that % &ctx1 == listener_obj.unblocked_context);
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  // Exercise 4
  listener_obj.reset();
  ctx2.unblock();

  // Verify 4
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());
  expect(that % &ctx1 == mutex.owner());
  expect(that % &ctx2 == listener_obj.unblocked_context);
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  // Exercise
  listener_obj.reset();
  future2.resume();

  // Verify 4: ctx2 is re-blocked by sync because ctx1 still has the lock
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % &ctx1 == mutex.owner());
  expect(that % nullptr == listener_obj.unblocked_context);
  expect(that % &ctx2 == listener_obj.sync_blocked);
  expect(that % &ctx1 == listener_obj.sync_blocker);

  // Exercise 5
  listener_obj.reset();
  ctx1.unblock();    // unblock the time based wait
  future1.resume();  // finishes and releases lock

  // Verify 5
  expect(that % future1.done());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % nullptr == mutex.owner());
  expect(that % &ctx1 == listener_obj.unblocked_context);
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  // Exercise 6
  listener_obj.reset();
  ctx2.unblock();
  future2.resume();  // acquires lock blocks by time

  // Verify 6
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::time == ctx2.state());
  expect(that % 1ms == ctx2.sleep_time());
  expect(that % &ctx2 == listener_obj.unblocked_context);
  expect(that % &ctx2 == mutex.owner());
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  // Exercise 7
  listener_obj.reset();
  ctx2.unblock();
  future2.resume();  // finishes and releases lock

  // Verify 7
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());
  expect(that % future1.done());
  expect(that % future2.done());
  expect(that % nullptr == mutex.owner());
  expect(that % &ctx2 == listener_obj.unblocked_context);
  expect(that % nullptr == listener_obj.sync_blocked);
  expect(that % nullptr == listener_obj.sync_blocker);

  ctx1.clear_listener();
  ctx2.clear_listener();
};

int main()
{
  context_listener_test();
}
