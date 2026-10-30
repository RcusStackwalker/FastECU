#pragma once
#include <chrono>
#include <cstddef>

#include "src/backend/ports/testing/fake_clock.h"

namespace fastecu
{

// Fails the `fail_at`-th Sleep (1-based) with kDisconnected "sleep failed";
// with 0 it only counts them.
class FaultingClock final : public FakeClock
{
  public:
    std::size_t fail_at = 0;
    std::size_t sleeps = 0;

    Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        if (++sleeps == fail_at)
        {
            return Fail(ErrorKind::kDisconnected, "sleep failed");
        }
        return FakeClock::Sleep(duration, cancellation);
    }
};

} // namespace fastecu
