#include <cassert>

#include <chrono>
#include <coroutine>
#include <memory>
#include <print>
#include <variant>
#include <vector>

#if not __ARM_EABI__
#include <thread>
#endif

import async_context;

// Simulates reading sensor data with I/O delay
async::future<int> read_sensor(async::context& ctx, std::string_view p_name)
{
  using namespace std::chrono_literals;
  co_await ctx.block_by_io();  // Simulate I/O operation
  co_return 42;
}

// Processes data with computation delay
async::future<int> process_data(async::context& ctx,
                                std::string_view p_name,
                                int value)
{
  using namespace std::chrono_literals;
  co_await 10ms;  // Simulate processing time
  int result = value * 2;
  co_return result;
}

// Writes result with I/O delay
async::future<void> write_actuator(async::context& ctx,
                                   std::string_view p_name,
                                   int value)
{
  co_await ctx.block_by_io();
}

// Coordinates the full pipeline
async::future<void> sensor_pipeline(async::context& ctx,
                                    std::string_view p_name)
{

  int sensor_value = co_await read_sensor(ctx, p_name);
  int processed = co_await process_data(ctx, p_name, sensor_value);
  co_await write_actuator(ctx, p_name, processed);

}

// Type-erased future wrapper for storing different future types
struct future_wrapper
{
  virtual void resume() = 0;
  [[nodiscard]] virtual bool done() const = 0;
  virtual ~future_wrapper() = default;
};

template<typename T>
class typed_future_wrapper : public future_wrapper
{
public:
  explicit typed_future_wrapper(async::future<T>&& p_future)
    : m_future(std::move(p_future))
  {
  }

  void resume() override
  {
    m_future.resume();
  }

  [[nodiscard]] bool done() const override
  {
    return m_future.done();
  }

private:
  async::future<T> m_future;
};

template<typename T>
auto make_future_wrapper(async::future<T>&& p_future)
{
  return std::make_unique<typed_future_wrapper<T>>(std::move(p_future));
}

template<std::size_t ContextCount = 2, std::size_t ContextSize = 1024>
struct round_robin_scheduler
{
  bool resume_n(int p_iterations)
  {
    for (int i = 0; i < p_iterations; i++) {
      bool all_done = true;
      for (auto& ctx : std::span(m_context_list).first(m_context_size)) {
        if (not ctx.done()) {
          all_done = false;
          if (ctx.state() == async::blocked_by::nothing) {
            ctx.resume();
            std::this_thread::sleep_for(ctx.sleep_time());
            ctx.unblock();  // simulate quick unblocking of the context.
          }
        }
      }
      if (all_done) {
        return true;
      }
    }
    return false;
  }

  template<typename AsyncOperation, typename... OperationArgs>
  void schedule_operation(AsyncOperation&& p_async_operation,
                          OperationArgs... args)
  {

    auto future = p_async_operation(m_context_list[m_context_size], args...);
    auto& f_wrap = future_list[m_context_size];
    f_wrap = make_future_wrapper(std::move(future));
    m_context_size++;
  }

  std::array<async::inplace_context<ContextSize>, ContextCount>
    m_context_list{};
  std::array<std::unique_ptr<future_wrapper>, ContextCount> future_list{};
  unsigned m_context_size = 0;
};

int main()
{
  round_robin_scheduler scheduler;

  // Run two independent pipelines concurrently
  scheduler.schedule_operation(sensor_pipeline, "🌟 System 1");
  scheduler.schedule_operation(sensor_pipeline, "🔥 System 2");

  scheduler.resume_n(100);

  return 0;
}
