#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

async::context const* sync_blocker = nullptr;
async::context* sync_blocked = nullptr;

void guards_tests()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  // Setup
  struct listener : public async::context_listener
  {
    void on_sync_block(async::context& p_blocked,
                       async::context const& p_blocker) noexcept override
    {

      std::println("✉️ on_sync_block");
      sync_blocker = &p_blocker;
      sync_blocked = &p_blocked;
    }
  };

  async::inplace_context<1024> ctx1;
  async::inplace_context<1024> ctx2;

  async::mutex mutex;

  listener test_listener;
  ctx1.set_listener(&test_listener);
  ctx2.set_listener(&test_listener);

  auto single_resource = [&](async::context& p_context) -> async::future<void> {
    std::println("Executing 'single_resource' coroutine");

    // Acquire guard for this scope
    auto guard = co_await mutex.lock(p_context);

    // setup dma transaction...
    std::println("Waiting on io complete flag, blocking by I/O");

    // Would normally wrap this in a while loop to check if the resource is
    // fread.
    co_await p_context.block_by_signal();

    // Normally NO cleanup would should be done at this point as it could
    // become a race condition.
    std::println("IO operation complete! Returning!");
    co_return;
  };

  // Exercise
  expect(that % 0 == ctx1.memory_used());
  expect(that % 0 == ctx2.memory_used());

  std::println("🧱 Future setup");
  auto access_first = single_resource(ctx1);
  auto access_second = single_resource(ctx2);

  expect(that % 0 < ctx1.memory_used());
  expect(that % 0 < ctx2.memory_used());
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());

  // access_first will claim the resource and will return control, and be
  // blocked by IO.
  std::println("▶️ [1] Resume 1st");
  access_first.resume();
  expect(that % async::blocked_by::signal == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());
  expect(that % nullptr == sync_blocker);
  expect(that % nullptr == sync_blocked);

  std::println("▶️ [2] Resume 2nd");
  access_second.resume();
  expect(that % async::blocked_by::signal == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % &ctx1 == sync_blocker);
  expect(that % &ctx2 == sync_blocked);
  sync_blocker = nullptr;
  sync_blocked = nullptr;

  std::println("🟢 [3] Unblock 2nd Context");
  ctx2.unblock();
  expect(that % async::blocked_by::signal == ctx1.state());
  expect(that % async::blocked_by::nothing == ctx2.state());

  std::println("▶️ [4] Resume 2nd, should re-block on sync");
  access_second.resume();
  expect(that % async::blocked_by::signal == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());
  expect(that % &ctx1 == sync_blocker);
  expect(that % &ctx2 == sync_blocked);
  sync_blocker = nullptr;
  sync_blocked = nullptr;

  std::println("🟢 [5] Unblock & Release Exclusive");
  mutex.unblock_and_release();
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % async::blocked_by::sync == ctx2.state());

  std::println("▶️ [6] Resume 1st, this should finish the operation");
  access_first.resume();
  expect(that % async::blocked_by::nothing == ctx1.state());
  expect(that % ctx1.done());
  expect(that % async::blocked_by::sync == ctx2.state());

  std::println("🟢 [7] Unblock context 2");
  ctx2.unblock_without_notification();
  std::println("▶️ [7] Resume 2nd, should block by signal");
  access_second.resume();
  expect(that % async::blocked_by::signal == ctx2.state());

  std::println("🟢 [8] Unblock & Release Exclusive");
  mutex.unblock_and_release();
  expect(that % async::blocked_by::nothing == ctx2.state());
  expect(that % not ctx2.done());

  std::println("▶️ [9] Resume 2nd should complete");
  access_second.resume();
  expect(that % ctx2.state() == async::blocked_by::nothing);
  expect(that % access_second.done());

  expect(that % 0 == ctx1.memory_used());
  expect(that % 0 == ctx2.memory_used());
};

int main()
{
  guards_tests();
}
