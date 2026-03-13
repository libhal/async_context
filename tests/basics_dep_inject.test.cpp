#include <coroutine>
#include <optional>

#include <boost/ut.hpp>
#include <utility>

import async_context;

void basics_dep_inject()
{
  using namespace boost::ut;

  "sync return type void"_test = []() {
    // Setup
    std::array<async::uptr, 1024> stack{};
    async::context ctx{};
    ctx.initialize_stack_memory(stack);

    unsigned step = 0;
    auto sync_coroutine = [&step](async::context&) -> async::future<void> {
      step = 1;
      return {};
    };

    // Exercise
    auto future = sync_coroutine(ctx);

    // Verify
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % 1 == step);
  };

  "suspend then co_return"_test = []() {
    // Setup

    std::array<async::uptr, 1024> stack{};
    async::context ctx{};
    ctx.initialize_stack_memory(stack);

    static constexpr int expected_return_value = 1413;
    unsigned step = 0;
    auto async_coroutine =
      [&step](async::context& p_ctx) -> async::future<int> {
      step = 1;
      co_await p_ctx.block_by_io();
      step = 2;
      co_return expected_return_value;
    };
    std::optional<async::blocked_by> block{};
    std::optional<async::block_info> info{};
    auto handler = [&block, &info](async::blocked_by p_state,
                                   async::block_info p_info) noexcept {
      block = p_state;
      info = p_info;
    };

    // Exercise 1
    auto future = async_coroutine(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);
    expect(block == std::nullopt);

    // Exercise 2: start and suspend coroutine
    ctx.resume(handler);

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 1 == step);
    expect(block.value() == async::blocked_by::io);

    // Exercise 3: resume and co_return from coroutine
    ctx.unblock();
    ctx.resume(handler);

    // Verify 3
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 2 == step);
  };

  "Call handler"_test = []() {
    // Setup

    std::array<async::uptr, 1024> stack{};
    async::context ctx{};
    ctx.initialize_stack_memory(stack);

    static constexpr int expected_return_value = 1413;
    unsigned step = 0;
    auto async_coroutine =
      [&step](async::context& p_ctx) -> async::future<int> {
      step = 1;
      co_await p_ctx.block_by_io();
      step = 2;
      co_return expected_return_value;
    };
    std::optional<async::blocked_by> block{};
    std::optional<async::block_info> info{};
    auto handler = [&block, &info](async::blocked_by p_state,
                                   async::block_info p_info) noexcept {
      block = p_state;
      std::println("State updated!");
      info = p_info;
    };

    // Exercise 1
    auto future = async_coroutine(ctx);

    // Verify 1
    expect(that % 0 < ctx.memory_used());
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 0 == step);

    // Exercise 2: start and suspend coroutine
    ctx.resume(handler);

    // Verify 2
    expect(that % 0 < ctx.memory_used());
    expect(async::blocked_by::io == block.value())
      << "Expected: " << static_cast<int>(async::blocked_by::io)
      << ", Got: " << static_cast<int>(block.value());
    // expect(that % async::block_by::io == *info);
    expect(that % not future.done());
    expect(that % not future.has_value());
    expect(that % 1 == step);

    // Exercise 3: resume and co_return from coroutine
    ctx.resume(handler);

    // Verify 3
    expect(that % 0 == ctx.memory_used());
    expect(that % future.done());
    expect(that % future.has_value());
    expect(that % expected_return_value == future.value());
    expect(that % 2 == step);
  };
};

int main()
{
  basics_dep_inject();
}
