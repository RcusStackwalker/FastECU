#pragma once
#include <atomic>
#include "src/backend/ports/cancellation.h"

// Thread-safe cancellation flag settable from the GUI thread and polled by the
// backend running on a worker thread.
class QtCancellationToken : public fastecu::ICancellationToken
{
  public:
    bool cancelled() const override
    {
        return flag_.load(std::memory_order_relaxed);
    }
    void cancel()
    {
        flag_.store(true, std::memory_order_relaxed);
    }
    void reset()
    {
        flag_.store(false, std::memory_order_relaxed);
    }

  private:
    std::atomic<bool> flag_{false};
};
