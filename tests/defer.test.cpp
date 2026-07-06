#include <coroutine>

#include <boost/ut.hpp>
#include <print>

import async_context;
import test_utils;

void defer_tests()
{
  using namespace boost::ut;

  // Setup
  async::inplace_context<1024> ctx;

  bool cleanup_ran = false;

  auto coroutine = [&](async::context&) -> async::future<void> {
    co_await async::defer([&](async::context&) -> async::future<void> {
      cleanup_ran = true;
      co_return;
    });
    co_return;
  };

  // Exercise
  auto future = coroutine(ctx);
  future.resume();

  // Verify
  expect(that % future.done());
  expect(that % cleanup_ran);
};

int main()
{
  defer_tests();
}
