#pragma once
#include "src/backend/ports/clock.h"

// Monotonic clock backed by QElapsedTimer; cancellable sleep polls the token
// in short slices so teardown unblocks promptly.
class QtClock : public fastecu::IClock
{
  public:
    std::uint64_t now_ms() const override;
    fastecu::Status sleep(int ms, const fastecu::ICancellationToken&) override;
};
