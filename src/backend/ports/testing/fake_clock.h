#pragma once
#include <cstdint>
#include <optional>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"

namespace fastecu
{

// A deterministic clock for tests: now advances only when told; sleep is
// instantaneous (no real wall-clock wait) but honours the cancellation token.
class FakeClock : public IClock
{
  public:
    std::uint64_t now_ms() const override
    {
        const auto value = now_;
        now_ += now_auto_advance_ms_;
        return value;
    }
    Status sleep(int ms, const ICancellationToken& t) override
    {
        if (t.cancelled())
        {
            return fail(ErrorKind::Cancelled);
        }
        now_ += sleep_advance_ms_.value_or(static_cast<std::uint64_t>(ms < 0 ? 0 : ms));
        return {};
    }

    void set_now_auto_advance_ms(std::uint64_t step_ms)
    {
        now_auto_advance_ms_ = step_ms;
    }

    void set_sleep_advance_ms(std::optional<std::uint64_t> step_ms)
    {
        sleep_advance_ms_ = step_ms;
    }

    mutable std::uint64_t now_ = 0;

  private:
    mutable std::uint64_t now_auto_advance_ms_ = 0;
    std::optional<std::uint64_t> sleep_advance_ms_;
};

inline FakeClock make_auto_advancing_clock(std::uint64_t step_ms)
{
    FakeClock clock;
    clock.set_now_auto_advance_ms(step_ms);
    clock.set_sleep_advance_ms(step_ms);
    return clock;
}

} // namespace fastecu
