#include <coroutine>
#include <optional>
#include <print>

#include <boost/ut.hpp>

import async_context;
import test_utils;

void cross_context_await_test()
{
  using namespace boost::ut;
  using namespace std::chrono_literals;

  // -------------------------------------------------------------------------
  // Normal completion: producer suspends once, then completes.
  // Verifies value propagation and memory cleanup.
  //
  // Resume sequence:
  //   fa.resume()   -> consumer step=10, starts producer on ctx_b -> step=1,
  //                    producer suspends. ctx_a is now cross-awaiting ctx_b.
  //   ctx_b.resume() -> producer step=2, co_return 42. ctx_b done.
  //   fa.resume()   -> ctx_b done, consumer picks up 42, step=11, completes.
  // -------------------------------------------------------------------------
  "cross-context co_await: normal completion propagates value"_test = []() {
    async::inplace_context<512> ctx_a;
    async::inplace_context<512> ctx_b;
    unsigned step = 0;

    auto producer = [&step](async::context&) -> async::future<int> {
      step = 1;
      co_await std::suspend_always{};
      step = 2;
      co_return 42;
    };

    auto consumer =
      [&step, &producer, &ctx_b](async::context&) -> async::future<int> {
      step = 10;
      int result = co_await producer(ctx_b);
      step = 11;
      co_return result;
    };

    auto fa = consumer(ctx_a);

    expect(that % 0 == step);
    expect(that % not fa.done());
    expect(that % 0 < ctx_a.memory_used());

    fa.resume();
    expect(that % 1 == step);
    expect(that % not fa.done());
    expect(that % not ctx_b.done());
    expect(that % 0 < ctx_b.memory_used());

    ctx_b.resume();
    expect(that % 2 == step);
    expect(that % ctx_b.done());
    expect(that % 0 == ctx_b.memory_used());

    fa.resume();
    expect(that % 11 == step);
    expect(that % fa.done());
    expect(that % fa.has_value());
    expect(that % 42 == fa.value());
    expect(that % 0 == ctx_a.memory_used());
  };

  // -------------------------------------------------------------------------
  // State delegation: while ctx_a is cross-awaiting ctx_b,
  // ctx_a.state() and ctx_a.sleep_time() must delegate to ctx_b.
  // -------------------------------------------------------------------------
  "cross-context co_await: state() and sleep_time() delegate to awaited context"_test =
    []() {
      using namespace std::chrono_literals;
      async::inplace_context<512> ctx_a;
      async::inplace_context<512> ctx_b;

      auto producer = [](async::context&) -> async::future<void> {
        co_await 50ms;
        co_return;
      };

      auto consumer = [&producer,
                       &ctx_b](async::context&) -> async::future<void> {
        co_await producer(ctx_b);
        co_return;
      };

      auto fa = consumer(ctx_a);

      // consumer starts producer on ctx_b, producer blocks by time
      fa.resume();

      expect(that % async::blocked_by::time == ctx_b.state());
      // ctx_a must delegate to ctx_b while cross-awaiting
      expect(that % async::blocked_by::time == ctx_a.state());
      expect(that % ctx_b.sleep_time() == ctx_a.sleep_time());
    };

  // -------------------------------------------------------------------------
  // Back-link cleared after normal completion.
  //
  // After the cross-context await resolves, await_resume must clear both
  // m_awaited_context and m_awaiting_caller. If it does not, ctx_a.state()
  // will delegate to the finished ctx_b (returning nothing) instead of
  // reporting its own blocked_by::time — this test catches that failure.
  //
  // Producer completes immediately (no intermediate suspension), so:
  //   fa.resume() #1 -> consumer step=1, producer runs to completion,
  //                     symmetric transfer to noop, resume returns.
  //                     Consumer is still suspended at co_await.
  //   fa.resume() #2 -> consumer sees ctx_b done, await_resume clears links,
  //                     consumer step=2, blocks by time on ctx_a.
  // -------------------------------------------------------------------------
  "cross-context co_await: back-links cleared after normal completion"_test =
    []() {
      using namespace std::chrono_literals;
      async::inplace_context<512> ctx_a;
      async::inplace_context<512> ctx_b;
      unsigned step = 0;

      auto producer = [](async::context&) -> async::future<void> {
        co_return;  // completes on first resume, no intermediate suspension
      };

      auto consumer =
        [&step, &producer, &ctx_b](async::context&) -> async::future<void> {
        step = 1;
        co_await producer(ctx_b);
        step = 2;
        co_await 50ms;  // must block ctx_a by time, not delegate to finished
                        // ctx_b
        step = 3;
        co_return;
      };

      auto fa = consumer(ctx_a);

      // producer completes immediately; symmetric transfer to noop returns
      // control consumer is still suspended at co_await — needs a second resume
      fa.resume();
      expect(that % 1 == step);
      expect(that % ctx_b.done());
      expect(that % 0 == ctx_b.memory_used());
      expect(that % not fa.done());
      expect(that % async::blocked_by::nothing == ctx_a.state());

      // consumer resumes past co_await, step=2, blocks by time
      fa.resume();
      expect(that % 2 == step);
      // if m_awaited_context was not cleared, this returns blocked_by::nothing
      // (delegating to the finished ctx_b) — the test catches that failure
      expect(that % async::blocked_by::time == ctx_a.state());
      expect(that % 50ms == ctx_a.sleep_time());
      expect(that % not fa.done());

      // consumer resumes past co_await, step=2, blocks by time
      ctx_a.unblock();
      fa.resume();
      expect(that % 3 == step);
      expect(that % fa.done());
    };

  // -------------------------------------------------------------------------
  // Safety: awaited context destroyed while caller is suspended.
  //
  // ctx_b is destroyed while ctx_a is cross-awaiting it. ctx_b's destructor
  // must null out ctx_a.m_awaited_context via the m_awaiting_caller back-link.
  // After destruction, ctx_a.state() must reflect its own state (nothing),
  // and resuming ctx_a must deliver future<int>::cancelled to the consumer's
  // try/catch block.
  // -------------------------------------------------------------------------
  "cross-context co_await: awaited context destroyed clears caller pointer"_test =
    []() {
      async::inplace_context<512> ctx_a;
      unsigned step = 0;
      bool cancelled_caught = false;

      // held in optional so we can destroy it independently of ctx_a
      std::optional<async::inplace_context<256>> ctx_b;
      ctx_b.emplace();

      auto producer = [&step](async::context&) -> async::future<int> {
        step = 1;
        co_await 50ms;
        step = 2;  // never reached
        co_return 42;
      };

      auto consumer = [&](async::context&) -> async::future<void> {
        step = 10;
        try {
          [[maybe_unused]] int result = co_await producer(*ctx_b);
          step = 11;  // never reached
        } catch (async::future<int>::cancelled const&) {
          cancelled_caught = true;
          step = 99;
        }
        co_return;
      };

      auto fa = consumer(ctx_a);

      // consumer step=10, producer on ctx_b step=1, ctx_a cross-awaiting ctx_b
      fa.resume();
      expect(that % 1 == step);
      expect(that % not fa.done());
      expect(that % not ctx_b->done());
      // ctx_a delegates state through to ctx_b context
      expect(that % async::blocked_by::time == ctx_a.state());

      // Destroy ctx_b while ctx_a is suspended awaiting it.
      // Expected: ctx_b.~inplace_context() calls cancel(), which sets the
      // producer future to cancelled_state, then the destructor nulls
      // ctx_a.m_awaited_context via m_awaiting_caller.
      ctx_b.reset();

      // ctx_a must no longer delegate state through the destroyed context
      expect(that % async::blocked_by::nothing == ctx_a.state());

      // Resuming ctx_a: await_resume sees cancelled_state, throws
      // future<int>::cancelled, caught inside consumer, step=99
      fa.resume();
      expect(that % 99 == step);
      expect(that % async::blocked_by::nothing == ctx_a.state());
      expect(that % true == cancelled_caught);
      expect(that % fa.done());
      expect(that % 0 == ctx_a.memory_used());
    };
}

int main()
{
  cross_context_await_test();
}
