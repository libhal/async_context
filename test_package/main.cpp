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

#include <array>
#include <chrono>
#include <coroutine>
#include <memory_resource>
#include <span>
#include <variant>

import async_context;

struct my_scheduler : public async::scheduler
{
  int sleep_count = 0;

private:
  void do_schedule(
    [[maybe_unused]] async::context& p_context,
    [[maybe_unused]] async::blocked_by p_block_state,
    [[maybe_unused]] std::variant<async::sleep_duration, async::context*>
      p_block_info) override
  {
    if (std::holds_alternative<async::sleep_duration>(p_block_info)) {
      sleep_count++;
    }
  }
};

async::future<void> coro_double_delay(async::context&)
{
  using namespace std::chrono_literals;
  co_await 100ns;
  co_await 100ns;
  co_return;
}
int main()
{
  auto scheduler =
    mem::make_strong_ptr<my_scheduler>(std::pmr::new_delete_resource());
  auto buffer = mem::make_strong_ptr<std::array<async::u8, 1024>>(
    std::pmr::new_delete_resource());
  auto buffer_span = mem::make_strong_ptr<std::span<async::u8>>(
    std::pmr::new_delete_resource(), *buffer);
  async::context my_context(scheduler, buffer_span);

  auto future_delay = coro_double_delay(my_context);

  assert(not future_delay.done());

  future_delay.resume();

  assert(scheduler->sleep_count == 1);

  future_delay.resume();

  assert(scheduler->sleep_count == 2);
  assert(not scheduler->done());

  future_delay.resume();

  assert(scheduler->done());

  return 0;
}
