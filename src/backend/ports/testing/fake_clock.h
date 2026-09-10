#pragma once
#include <chrono>
#include <optional>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"

namespace fastecu
{

// A deterministic clock for tests: time advances only when told; sleep is
// instantaneous (no real wall-clock wait) but honours the cancellation token.
//
// Elapsed time is stored as a duration and the instant derived from it, so
// tests assert on elapsed() and never do time_point arithmetic.
class FakeClock : public IClock
{
  public:
    std::chrono::steady_clock::time_point now() const override
    {
        const auto value = elapsed_;
        elapsed_ += now_auto_advance_;
        return std::chrono::steady_clock::time_point{} + value;
    }

    Status sleep(std::chrono::milliseconds duration, const ICancellationToken& t) override
    {
        if (t.cancelled())
        {
            return fail(ErrorKind::Cancelled);
        }
        elapsed_ += sleep_advance_.value_or(
            duration < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : duration);
        return {};
    }

    std::chrono::milliseconds elapsed() const
    {
        return elapsed_;
    }

    void set_now_auto_advance(std::chrono::milliseconds step)
    {
        now_auto_advance_ = step;
    }

    void set_sleep_advance(std::optional<std::chrono::milliseconds> step)
    {
        sleep_advance_ = step;
    }

  private:
    mutable std::chrono::milliseconds elapsed_{0};
    mutable std::chrono::milliseconds now_auto_advance_{0};
    std::optional<std::chrono::milliseconds> sleep_advance_;
};

inline FakeClock make_auto_advancing_clock(std::chrono::milliseconds step)
{
    FakeClock clock;
    clock.set_now_auto_advance(step);
    clock.set_sleep_advance(step);
    return clock;
}

} // namespace fastecu
