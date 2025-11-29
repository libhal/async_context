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
#include <memory_resource>
#include <span>

#include <boost/ut.hpp>

import async_context;

namespace async {
boost::ut::suite<"async::context"> adc24_test = []() {
  using namespace boost::ut;

  "<TBD>"_test = []() {
    class my_scheduler : public async::scheduler
    {
    private:
      void do_schedule([[maybe_unused]] context& p_context,
                       [[maybe_unused]] blocked_by p_block_state,
                       [[maybe_unused]] std::variant<sleep_duration, context*>
                         p_block_info) override
      {
      }
    };
    // Setup
    auto scheduler =
      mem::make_strong_ptr<my_scheduler>(std::pmr::new_delete_resource());
    auto buffer = mem::make_strong_ptr<std::array<async::u8, 1024>>(
      std::pmr::new_delete_resource());
    auto buffer_span = mem::make_strong_ptr<std::span<async::u8>>(
      std::pmr::new_delete_resource(), *buffer);
    async::context my_context(scheduler, buffer_span);
    // Exercise
    // Verify
  };
};
}  // namespace async
