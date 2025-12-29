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

struct test_scheduler
  : public async::scheduler
  , mem::enable_strong_from_this<test_scheduler>
{
  int sleep_count = 0;

  test_scheduler(mem::strong_ptr_only_token)
  {
  }

private:
  void do_schedule([[maybe_unused]] async::context& p_context,
                   [[maybe_unused]] async::blocked_by p_block_state,
                   [[maybe_unused]] async::scheduler::block_info
                     p_block_info) noexcept override
  {
    if (std::holds_alternative<std::chrono::nanoseconds>(p_block_info)) {
      sleep_count++;
    }
  }

  std::pmr::memory_resource& do_get_allocator() noexcept override
  {
    return *strong_from_this().get_allocator();
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
  auto scheduler =
    mem::make_strong_ptr<test_scheduler>(std::pmr::new_delete_resource());
  async::context my_context(scheduler, 1024);

  auto future_delay = coro_double_delay(my_context);

  assert(not future_delay.done());

  future_delay.resume();

  assert(scheduler->sleep_count == 1);

  future_delay.resume();

  assert(scheduler->sleep_count == 2);
  assert(not future_delay.done());

  future_delay.resume();

  assert(future_delay.done());

  return 0;
}
