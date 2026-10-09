#pragma once
#include "src/backend/ports/testing/fake_clock.h"

#include <chrono>
#include <vector>

namespace fastecu
{

// A FakeClock that records every requested sleep duration, in call order,
// before delegating. A cancelled sleep is still recorded.
//
// For asserting the exact ordered list of sleeps. To assert that a sleep
// happens (n times) or not, or to fail or trigger something on one, use
// MockClock rather than deriving from this or from FakeClock.
class RecordingClock final : public FakeClock
{
  public:
    Status Sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(duration);
        return FakeClock::Sleep(duration, cancellation);
    }

    std::vector<std::chrono::milliseconds> sleep_calls;
};

} // namespace fastecu
