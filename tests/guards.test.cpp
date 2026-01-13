#include <coroutine>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

boost::ut::suite<"guards_tests"> guards_tests = []() {
  using namespace boost::ut;
  using namespace std::chrono_literals;
  "Context Token"_test = []() {
    // Setup
    test_context ctx1;
    test_context ctx2;

    async::context_token io_in_use;

    auto single_resource =
      [&](async::context& p_context) -> async::future<void> {
      using namespace std::chrono_literals;

      std::println("Executing 'single_resource' coroutine");
      while (io_in_use) {
        // TODO(#44): For some reason this segfaults on Linux
        // std::println("Resource unavailable, blocked by {}",
        //              io_in_use.address());
        co_await io_in_use.set_as_block_by_sync(p_context);
      }

      // Block next coroutine from using this resource
      io_in_use = p_context;

      // setup dma transaction...

      // It cannot be assumed that the scheduler will not sync_wait() this
      // coroutine, thus, a loop is required to sure that the async operation
      // has actually completed.
      while (io_in_use == p_context) {
        std::println("Waiting on io complete flag, blocking by I/O");
        // Continually notify that this is blocked by IO
        co_await p_context.block_by_io();
      }

      std::println("IO operation complete! Returning!");

      co_return;
    };

    std::println("🧱 Future setup");
    auto access_first = single_resource(ctx1);
    auto access_second = single_resource(ctx2);

    expect(that % 0 < ctx1.memory_used());
    expect(that % 0 < ctx2.memory_used());

    auto check_access_first_blocked_by =
      [&](async::blocked_by p_state = async::blocked_by::io,
          std::source_location const& p_location =
            std::source_location::current()) {
        expect(that % p_state == ctx1.state())
          << "ctx1 state mismatch, line: " << p_location.line() << '\n';
      };

    auto check_access_second_blocked_by =
      [&](async::blocked_by p_state = async::blocked_by::nothing,
          std::source_location const& p_location =
            std::source_location::current()) {
        expect(that % p_state == ctx2.state())
          << "ctx2 state mismatch, line: " << p_location.line() << '\n';
      };

    // access_first will claim the resource and will return control, and be
    // blocked by IO.
    std::println("▶️ Resume 1st: 1");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 1st: 2");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 1st: 3");
    access_first.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by();

    std::println("▶️ Resume 2nd: 1");
    access_second.resume();

    check_access_first_blocked_by();
    check_access_second_blocked_by(async::blocked_by::sync);

    io_in_use.unblock_and_clear();

    check_access_first_blocked_by(async::blocked_by::nothing);
    check_access_second_blocked_by(async::blocked_by::sync);

    std::println("▶️ Resume 2nd: 2");
    access_second.resume();

    // Resuming access_second shouldn't change the state of anything
    check_access_first_blocked_by(async::blocked_by::nothing);
    check_access_second_blocked_by(async::blocked_by::io);

    std::println("▶️ Resume 1st: 4, this should finish the operation");
    access_first.resume();

    expect(that % ctx1.state() == async::blocked_by::nothing);
    expect(that % access_first.done());

    check_access_second_blocked_by(async::blocked_by::io);
    access_second.resume();
    check_access_second_blocked_by(async::blocked_by::io);

    io_in_use.unblock_and_clear();
    access_second.resume();

    expect(that % ctx2.state() == async::blocked_by::nothing);
    expect(that % access_second.done());

    expect(that % 0 == ctx1.memory_used());
    expect(that % 0 == ctx2.memory_used());
  };
};
