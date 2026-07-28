#pragma once
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
        return now_;
    }
    Status sleep(int ms, const ICancellationToken& t) override
    {
        if (t.cancelled())
        {
            return fail(ErrorKind::Cancelled);
        }
        now_ += static_cast<std::uint64_t>(ms < 0 ? 0 : ms);
        return {};
    }

    std::uint64_t now_ = 0;
};

} // namespace fastecu
