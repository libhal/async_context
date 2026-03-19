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
#include <memory>
#include <print>
#include <ratio>
#include <variant>
#include <vector>

#if not __ARM_EABI__
#include <thread>
#endif

import async_context;
import async_context.schedulers;

using namespace std::chrono_literals;

// Simulates reading sensor data with I/O delay
async::future<int> read_sensor(async::context& ctx, std::string_view p_name)
{
  std::println("['{}': Sensor] Read complete: 42", p_name);
  co_return 42;
}

// Processes data with computation delay
async::future<int> process_data(async::context& p_ctx,
                                std::string_view p_name,
                                int value)
{
  std::println("['{}': Process] Processing {}...", p_name, value);
  co_await 10ms;  // Simulate processing time
  int result = value * 2;
  std::println("['{}': Process] Result: {}", p_name, result);
  co_return result;
}

async::future<void> write_actuator(async::context& p_ctx,
                                   std::string_view p_name,
                                   int value)
{
  std::println("['{}': Actuator] Writing {}...", p_name, value);
  co_await p_ctx.block_by_io();
  std::println("['{}': Actuator] Write complete!", p_name);
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

// Stub implementation for ARM M Cortex targets without
// std::chrono::steady_clock
struct stub_clock
{
  using duration = std::chrono::nanoseconds;
  using rep = duration::rep;
  using period = duration::period;
  using time_point = std::chrono::time_point<stub_clock>;
  static constexpr bool is_steady = false;

  static time_point now() noexcept
  {
    return time_point(duration(0));
  }
};

int main()
{
#if __ARM_EABI__
  async::chrono_clock_adapter<stub_clock> clk;
#else
  async::chrono_clock_adapter<std::chrono::steady_clock> clk;
#endif

  async::inplace_context<512> ctx0;
  async::inplace_context<512> ctx1;
  async::inplace_context<512> unblock_context;

  // Run two independent pipelines concurrently
  auto pipeline1_future = sensor_pipeline(ctx0, "🌟 System 1");
  auto pipeline2_future = sensor_pipeline(ctx1, "🔥 System 2");
  // This is needed to simulate the context being unblocked by an external
  // callback.
  auto unblock_function =
    [&ctx0, &ctx1](async::context& p_ctx) -> async::future<void> {
    while (true) {
      if (ctx0.done() and ctx1.done()) {
        break;
      }
      if (ctx0.state() == async::blocked_by::io) {
        ctx0.unblock();
      }
      if (ctx1.state() == async::blocked_by::io) {
        ctx1.unblock();
      }
      co_await 1us;
    }
  };

  auto unblock_future = unblock_function(unblock_context);

  auto stub_sleep_function = [](auto p_wake_time) {
#if __ARM_EABI__
    static_cast<void>(p_wake_time);  // ignore parameter
#else
    std::this_thread::sleep_until(p_wake_time);
#endif
  };

  // Run ctx0, ctx1 and unblock_context to completion
  async::run_until_done(clk, stub_sleep_function, ctx0, ctx1, unblock_context);

  std::println("Both pipelines completed successfully!");
  return 0;
}
