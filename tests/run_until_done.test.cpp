#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <future>
#include <mutex>
#include <print>
#include <thread>
#include <vector>

#include <boost/ut.hpp>

import async_context;
import async_context.schedulers;

using namespace std::chrono_literals;

async::task sleeping_task(async::context& p_ctx,
                          std::string_view p_name,
                          async::sleep_duration p_sleep_time)
{
  std::println("[{}] Starting...", p_name);

  for (int i = 1; i <= 5; i++) {
    co_await p_sleep_time;
    std::println("[{}] sleep {}", p_name, i);
  }
  co_return;
}

async::task blocking_task(async::context& p_ctx,
                          async::sleep_duration p_sleep_time)
{
  std::atomic_bool simulated_hardware_busy = true;

  auto t = std::jthread([&]() {
    std::this_thread::sleep_for(1000ms);
    std::println("Unblocking context: {}", static_cast<void*>(&p_ctx));
    simulated_hardware_busy = false;
    p_ctx.unblock();
  });

  co_await p_sleep_time;

  while (simulated_hardware_busy) {
    co_await p_ctx.block_by_io();
  }

  co_return;
}

void run_sleep_task_test()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  async::inplace_context<512> ctx0;
  async::inplace_context<512> ctx1;
  async::inplace_context<512> ctx2;
  async::inplace_context<512> ctx3;
  async::inplace_context<512> ctx4;
  async::inplace_context<512> ctx5;

  "run_until_done"_test = [&]() {
    // Setup
    async::chrono_clock_adapter<std::chrono::steady_clock> clk;

    std::vector<async::task> future_set;

    future_set.push_back(sleeping_task(ctx0, "A", 55ms));
    future_set.push_back(sleeping_task(ctx1, "B", 60ms));
    future_set.push_back(sleeping_task(ctx2, "C", 65ms));
    future_set.push_back(sleeping_task(ctx3, "D", 70ms));
    future_set.push_back(sleeping_task(ctx4, "E", 75ms));
    future_set.push_back(sleeping_task(ctx5, "F", 100ms));

    // Exercise
    async::run_until_done(
      clk,
      [](auto p_wake_time) { std::this_thread::sleep_until(p_wake_time); },
      // List of tasks
      ctx0,
      ctx1,
      ctx2,
      ctx3,
      ctx4,
      ctx5);

    // Verify
    expect(future_set[0].done()) << "Context 0 didn't finish!";
    expect(future_set[1].done()) << "Context 1 didn't finish!";
    expect(future_set[2].done()) << "Context 2 didn't finish!";
    expect(future_set[3].done()) << "Context 3 didn't finish!";
    expect(future_set[4].done()) << "Context 4 didn't finish!";
    expect(future_set[5].done()) << "Context 5 didn't finish!";
  };

  "run_until_done with unblock listener"_test = [&]() {
    // Setup
    async::chrono_clock_adapter<std::chrono::steady_clock> clk;
    std::vector<async::task> future_set;

    future_set.push_back(blocking_task(ctx0, 0ms));
    future_set.push_back(blocking_task(ctx1, 10ms));
    future_set.push_back(blocking_task(ctx2, 20ms));
    future_set.push_back(blocking_task(ctx3, 30ms));
    future_set.push_back(blocking_task(ctx4, 40ms));
    future_set.push_back(blocking_task(ctx5, 50ms));
    std::vector<async::context*> unblocked_contexts;
    std::mutex unblocked_mutex;

    // Exercise
    async::run_until_done(
      clk,
      [&clk](auto p_wake_time) {
        if (clk.now() > p_wake_time) {
          return;
        }
        std::println("Sleeping for {}", p_wake_time - clk.now());
        std::this_thread::sleep_until(p_wake_time);
      },
      async::unblock_listener::from([&](async::context& p_ctx) noexcept {
        if (p_ctx.state() == async::blocked_by::io) {
          std::lock_guard lock(unblocked_mutex);
          unblocked_contexts.push_back(&p_ctx);
        }
      }),
      // List of tasks
      ctx0,
      ctx1,
      ctx2,
      ctx3,
      ctx4,
      ctx5);

    // Verify
    expect(future_set[0].done()) << "Context 0 didn't finish!";
    expect(future_set[1].done()) << "Context 1 didn't finish!";
    expect(future_set[2].done()) << "Context 2 didn't finish!";
    expect(future_set[3].done()) << "Context 3 didn't finish!";
    expect(future_set[4].done()) << "Context 4 didn't finish!";
    expect(future_set[5].done()) << "Context 5 didn't finish!";

    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx0));
    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx1));
    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx2));
    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx3));
    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx4));
    expect(that % 1 == std::ranges::count(unblocked_contexts, &ctx5));
  };
}

int main()
{
  run_sleep_task_test();
}
