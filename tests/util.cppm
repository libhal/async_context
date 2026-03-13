module;

#include <chrono>
#include <ostream>
#include <variant>

#include <boost/ut.hpp>

export module test_utils;

import async_context;

// NOLINTBEGIN(bugprone-exception-escape)
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
  struct sleep_counter
  {
    int count = 0;

    void operator()(async::sleep_duration)
    {
      count++;
    }
  };

  struct counter_pair
  {
    int constructed = 0;
    int destructed = 0;

    bool operator==(counter_pair const&) const = default;

    friend std::ostream& operator<<(std::ostream& out, counter_pair const& c)
    {
      return out << "{ constructed: " << c.constructed
                 << ", destructed: " << c.destructed << " }";
    }
  };

  struct raii_counter
  {
    raii_counter(counter_pair& p_counts, std::string_view p_label = "X")
      : m_counts(&p_counts)
      , m_label(p_label)
    {
      std::println("🟢 CTOR: {}", m_label);
      m_counts->constructed++;
    }

    ~raii_counter()  // NOLINT(bugprone-exception-escape)
    {
      std::println("🔵 DTOR: {}", m_label);
      m_counts->destructed++;
    }
    counter_pair* m_counts;
    std::string_view m_label;
  };
}
// NOLINTEND(bugprone-exception-escape)
