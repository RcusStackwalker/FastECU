#pragma once
#include "src/backend/ports/testing/fake_clock.h"

#include <chrono>
#include <vector>

namespace fastecu
{

// A FakeClock that records every requested sleep duration, in call order,
// before delegating. A cancelled sleep is still recorded.
//
// Deliberately not final: a test needing extra sleep-time behavior (a
// timeline, cancelling mid-sleep) derives from this and calls
// RecordingClock::sleep rather than keeping its own duration list.
class RecordingClock : public FakeClock
{
  public:
    Status sleep(std::chrono::milliseconds duration, const ICancellationToken& cancellation) override
    {
        sleep_calls.push_back(duration);
        return FakeClock::sleep(duration, cancellation);
    }

    std::vector<std::chrono::milliseconds> sleep_calls;
};

} // namespace fastecu
