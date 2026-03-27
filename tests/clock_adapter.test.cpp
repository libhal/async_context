#include <chrono>
#include <type_traits>

import async_context;

static_assert(
  std::is_empty_v<async::chrono_clock_adapter<std::chrono::steady_clock>>);
static_assert(
  async::clock<async::chrono_clock_adapter<std::chrono::steady_clock>>);

int main()
{
}
