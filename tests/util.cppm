module;

#include <chrono>
#include <ostream>
#include <variant>

#include <boost/ut.hpp>

export module test_utils;

import async_context;

export namespace async {
std::ostream& operator<<(std::ostream& out, blocked_by b)
{
  switch (b) {
    case blocked_by::nothing:
      return out << "nothing";
    case blocked_by::time:
      return out << "time";
    case blocked_by::io:
      return out << "io";
    case blocked_by::sync:
      return out << "sync";
    case blocked_by::external:
      return out << "external";
    default:
      // For unknown values we print the numeric value
      return out << "blocked_by(" << static_cast<std::uint8_t>(b) << ')';
  }
}
}  // namespace async

export {
  struct thread_info
  {
    async::context* sync_context = nullptr;
    int sleep_count = 0;
    bool io_block = false;
    async::sleep_duration last_sleep_time = {};
    bool scheduled_called_once = false;
  };

  struct test_context : public async::basic_context
  {
    std::shared_ptr<thread_info> info;
    std::array<async::uptr, 8192> m_stack{};

    test_context(std::shared_ptr<thread_info> const& p_info)
      : info(p_info)
    {
      this->initialize_stack_memory(m_stack);
    }
    test_context()
      : info(std::make_shared<thread_info>())
    {
      this->initialize_stack_memory(m_stack);
    }

    ~test_context() override
    {
      cancel();
    }

  private:
    void do_schedule(async::blocked_by p_block_state,
                     async::block_info p_block_info) noexcept override
    {
      info->scheduled_called_once = true;

      switch (p_block_state) {
        case async::blocked_by::time: {
          if (std::holds_alternative<std::chrono::nanoseconds>(p_block_info)) {
            auto const time = std::get<std::chrono::nanoseconds>(p_block_info);
            info->sleep_count++;
            info->last_sleep_time = time;
          }
          unblock_without_notification();
          break;
        }
        case async::blocked_by::sync: {
          if (std::holds_alternative<async::context*>(p_block_info)) {
            auto* context = std::get<async::context*>(p_block_info);
            std::println(
              "Coroutine ({}) is blocked by syncronization with coroutine ({})",
              static_cast<void*>(this),
              static_cast<void*>(context));
            info->sync_context = context;
          }
          break;
        }
        case async::blocked_by::io: {
          info->io_block = true;
          break;
        }
        case async::blocked_by::nothing: {
          std::println("Context ({}) has been unblocked!",
                       static_cast<void*>(this));
          break;
        }
        default: {
          break;
        }
      }
    }
  };

  struct sleep_counter
  {
    int count = 0;

    void operator()(async::sleep_duration)
    {
      count++;
    }
  };

  struct raii_counter
  {
    raii_counter(std::pair<int*, int*> p_counts, std::string_view p_label = "X")
      : counts(p_counts)
      , m_label(p_label)
    {
      std::println("🟢 CTOR: {}", m_label);
      (*counts.first)++;
    }

    ~raii_counter()  // NOLINT(bugprone-exception-escape)
    {
      std::println("🔵 DTOR: {}", m_label);
      (*counts.second)++;
    }
    std::pair<int*, int*> counts;
    std::string_view m_label;
  };
}
