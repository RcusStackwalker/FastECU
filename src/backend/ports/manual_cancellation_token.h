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
    // relaxed suffices: the flag publishes no other data (a pure signal, not
    // a guard for anything else the caller wrote), and FlashWorker's
    // teardown always pairs cancel() with transport_->request_unblock(),
    // which carries its own synchronization.
    std::atomic<bool> flag_{false};
};

} // namespace fastecu
