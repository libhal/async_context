#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void defer_tests()
{
  using namespace boost::ut;

  // Setup
  async::inplace_context<1024> ctx;

  bool cleanup_ran = false;

  struct obj
  {
    obj()
    {
      std::println("Created");
    }
    ~obj()
    {
      std::println("Destroyed");
    }
  };

  auto coroutine = [&](async::context&) -> async::future<void> {
    obj o;
    auto v = co_await async::defer([&](async::context&) -> async::future<void> {
      cleanup_ran = true;
      co_return;
    });
    co_return;
  };

  // Exercise
  auto future = coroutine(ctx);
  std::println("Before Resume");
  future.resume();
  std::println("After Resume");

  // Verify
  expect(that % future.done());
  expect(that % cleanup_ran);
};

int main()
{
  defer_tests();
}
