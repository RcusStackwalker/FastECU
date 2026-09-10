#pragma once
#include <chrono>

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

namespace fastecu
{

// Monotonic time source and cancellable delay. Replaces QElapsedTimer /
// QThread::msleep in backend code.
class IClock
{
  public:
    virtual ~IClock() = default;
    virtual std::chrono::steady_clock::time_point now() const = 0;
    // Returns Error{Cancelled} if the token trips before the delay elapses.
    virtual Status sleep(std::chrono::milliseconds duration, const ICancellationToken&) = 0;
};

} // namespace fastecu
