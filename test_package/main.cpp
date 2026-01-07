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
#include <cassert>

#include <chrono>
#include <coroutine>
#include <print>
#include <thread>
#include <variant>
#include <vector>

import async_context;

struct round_robin_scheduler
{
  bool run_until_all_done(int p_iterations = 100)
  {
    for (int i = 0; i < p_iterations; i++) {
      bool any_active = false;
      for (auto const& ctx : contexts) {
        if (ctx->state() == async::blocked_by::nothing) {
          any_active = true;
          ctx->resume();
        }
      }
      if (not any_active) {
        return true;
      }
    }
    return false;
  }
  std::vector<async::context*> contexts{};
};

struct test_context : public async::context
{
  std::array<async::uptr, 8192> m_stack{};
  round_robin_scheduler* scheduler = nullptr;

  test_context(round_robin_scheduler& sched)
    : scheduler(&sched)
  {
    this->initialize_stack_memory(m_stack);
  }

private:
  void do_schedule(async::blocked_by p_blocked_state,
                   async::block_info p_block_info) noexcept override
  {
    // Simulate I/O completion - immediately unblock
    if (p_blocked_state == async::blocked_by::io) {
      this->unblock_without_notification();
    } else if (p_blocked_state == async::blocked_by::time) {
      if (auto* time = std::get_if<std::chrono::nanoseconds>(&p_block_info)) {
        std::this_thread::sleep_for(*time);
      }
      this->unblock_without_notification();
    }
  }
};

// Simulates reading sensor data with I/O delay
async::future<int> read_sensor(async::context& ctx, std::string_view p_name)
{
  using namespace std::chrono_literals;
  std::println("  ['{}': Sensor] Starting read...", p_name);
  co_await ctx.block_by_io();  // Simulate I/O operation
  std::println("  ['{}': Sensor] Read complete: 42", p_name);
  co_return 42;
}

// Processes data with computation delay
async::future<int> process_data(async::context& ctx,
                                std::string_view p_name,
                                int value)
{
  using namespace std::chrono_literals;
  std::println("  ['{}': Process] Processing {}...", p_name, value);
  co_await 10ms;  // Simulate processing time
  int result = value * 2;
  std::println("  ['{}': Process] Result: {}", p_name, result);
  co_return result;
}

// Writes result with I/O delay
async::future<void> write_actuator(async::context& ctx,
                                   std::string_view p_name,
                                   int value)
{
  std::println("  ['{}': Actuator] Writing {}...", p_name, value);
  co_await ctx.block_by_io();
  std::println("  ['{}': Actuator] Write complete!", p_name);
}

// Coordinates the full pipeline
async::future<void> sensor_pipeline(async::context& ctx,
                                    std::string_view p_name)
{
  std::println("Pipeline '{}' starting...", p_name);

  int sensor_value = co_await read_sensor(ctx, p_name);
  int processed = co_await process_data(ctx, p_name, sensor_value);
  co_await write_actuator(ctx, p_name, processed);

  std::println("Pipeline '{}' complete!\n", p_name);
}

int main()
{
  round_robin_scheduler scheduler;

  test_context ctx1(scheduler);
  test_context ctx2(scheduler);

  scheduler.contexts.push_back(&ctx1);
  scheduler.contexts.push_back(&ctx2);

  // Run two independent pipelines concurrently
  auto pipeline1 = sensor_pipeline(ctx1, "System 1");
  auto pipeline2 = sensor_pipeline(ctx2, "System 2");

  scheduler.run_until_all_done();

  assert(pipeline1.done());
  assert(pipeline2.done());

  std::println("Both pipelines completed successfully!");
  return 0;
}
