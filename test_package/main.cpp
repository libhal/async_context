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
#include <memory_resource>
#include <print>
#include <variant>

import async_context;

struct test_context : public async::context
{
  std::array<async::uptr, 8192> m_stack{};
  int sleep_count = 0;
  test_context()
  {
    this->initialize_stack_memory(m_stack);
  }

private:
  void do_schedule(async::blocked_by p_blocked_state,
                   async::block_info) noexcept override
  {
    if (p_blocked_state == async::blocked_by::time) {
      sleep_count++;
    }
  }
};

async::future<void> coro_double_delay(async::context&)
{
  using namespace std::chrono_literals;
  std::println("Delay for 500ms");
  co_await 500ms;
  std::println("Delay for another 500ms");
  co_await 500ms;
  std::println("Returning!");
  co_return;
}

int main()
{
  test_context ctx;

  auto future_delay = coro_double_delay(ctx);

  assert(not future_delay.done());

  future_delay.resume();

  assert(ctx.sleep_count == 1);

  future_delay.resume();

  assert(ctx.sleep_count == 2);
  assert(not future_delay.done());

  future_delay.resume();

  assert(future_delay.done());

  return 0;
}
