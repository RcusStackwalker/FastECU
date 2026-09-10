#pragma once
#include "src/backend/ports/clock.h"

// Monotonic clock backed by QElapsedTimer; cancellable sleep polls the token
// in short slices so teardown unblocks promptly.
class QtClock : public fastecu::IClock
{
  public:
    std::chrono::steady_clock::time_point now() const override;
    fastecu::Status sleep(std::chrono::milliseconds duration, const fastecu::ICancellationToken&) override;
};
