#pragma once
// Shared IClock test double. Extracted from src/backend/ports/clock_test.cpp
// so cross-package tests (e.g. the Denso SH705x K-Line executor tests) can
// reuse the exact same deterministic clock instead of hand-rolling another
// one. Header-only; picked up automatically by the ":ports" cc_library glob.
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
