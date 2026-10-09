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
    std::chrono::steady_clock::time_point Now() const override
    {
        const auto value = elapsed_;
        elapsed_ += now_auto_advance_;
        return std::chrono::steady_clock::time_point{} + value;
    }

    Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& t) override
    {
        if (t.Cancelled())
        {
            return Fail(ErrorKind::kCancelled);
        }
        elapsed_ += sleep_advance_.value_or(
            duration < std::chrono::milliseconds::zero() ? std::chrono::milliseconds::zero() : duration);
        return {};
    }

    std::chrono::milliseconds Elapsed() const
    {
        return elapsed_;
    }

    void SetNowAutoAdvance(std::chrono::milliseconds step)
    {
        now_auto_advance_ = step;
    }

    void SetSleepAdvance(std::optional<std::chrono::milliseconds> step)
    {
        sleep_advance_ = step;
    }

  private:
    mutable std::chrono::milliseconds elapsed_{0};
    mutable std::chrono::milliseconds now_auto_advance_{0};
    std::optional<std::chrono::milliseconds> sleep_advance_;
};

inline FakeClock MakeAutoAdvancingClock(std::chrono::milliseconds step)
{
    FakeClock clock;
    clock.SetNowAutoAdvance(step);
    clock.SetSleepAdvance(step);
    return clock;
}

} // namespace fastecu
