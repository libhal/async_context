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

#include <array>
#include <chrono>
#include <coroutine>
#include <memory_resource>
#include <span>
#include <variant>

#include <boost/ut.hpp>

import async_context;

bool resumption_occurred = false;

async::future<void> coro_print(async::context&)
{
  using namespace std::chrono_literals;
  std::println("Printed from a coroutine");
  co_await 100ns;
  resumption_occurred = true;
  co_await 100ns;
  co_return;
}

namespace async {
boost::ut::suite<"async::context"> async_context_suite = []() {
  using namespace boost::ut;

  "<TBD>"_test = []() {
    struct my_scheduler
      : public async::scheduler
      , mem::enable_strong_from_this<my_scheduler>
    {
      int sleep_count = 0;

      my_scheduler(mem::strong_ptr_only_token)
      {
      }

    private:
      void do_schedule(
        [[maybe_unused]] context& p_context,
        [[maybe_unused]] blocked_by p_block_state,
        [[maybe_unused]] std::variant<std::chrono::nanoseconds, context*>
          p_block_info) override
      {
        std::println("Scheduler called!", sleep_count);
        if (std::holds_alternative<std::chrono::nanoseconds>(p_block_info)) {
          std::println("sleep for: {}",
                       std::get<std::chrono::nanoseconds>(p_block_info));
          sleep_count++;
          std::println("Sleep count = {}!", sleep_count);
        }
      }

      std::pmr::memory_resource& do_get_allocator() noexcept override
      {
        return *strong_from_this().get_allocator();
      }
    };
    // Setup
    auto scheduler =
      mem::make_strong_ptr<my_scheduler>(std::pmr::new_delete_resource());
    async::context my_context(scheduler, 1024);

    // Exercise
    auto future_print = coro_print(my_context);
    future_print.sync_wait();

    // Verify
    expect(that % resumption_occurred);
    expect(that % future_print.done());
    expect(that % 2 == scheduler->sleep_count);
  };
};
}  // namespace async
