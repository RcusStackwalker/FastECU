#pragma once
#include <atomic>
#include "src/backend/ports/cancellation.h"

namespace fastecu
{

// A cancellation flag flipped by the owning code (typically GUI-thread
// teardown) and polled from wherever the ICancellationToken& ends up --
// typically a worker thread running backend logic. One instance per
// operation attempt; construct a fresh one rather than reusing across runs.
class ManualCancellationToken final : public ICancellationToken
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

  private:
    std::atomic<bool> flag_{false};
};

} // namespace fastecu
