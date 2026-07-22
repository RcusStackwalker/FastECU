#pragma once
#include <cstdint>
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
    virtual std::uint64_t now_ms() const = 0;
    // Returns Error{Cancelled} if the token trips before the delay elapses.
    virtual Status sleep(int ms, const ICancellationToken&) = 0;
};

} // namespace fastecu
