#pragma once
#include <atomic>

#include "src/backend/ports/cancellation.h"

namespace fastecu::flash
{

// The concrete, flippable cancellation source this repo does not yet have
// under src/backend/ports. Platform code (FlashWorker, Task 11) owns one per
// execution attempt; portable code only ever sees the ICancellationToken&
// this class hands out.
class CancellationSource
{
  public:
    ICancellationToken& token()
    {
        return token_;
    }
    void trip()
    {
        flag_.store(true, std::memory_order_release);
    }

  private:
    class Token : public ICancellationToken
    {
      public:
        explicit Token(const std::atomic<bool>& flag) : flag_(flag)
        {
        }
        bool cancelled() const override
        {
            return flag_.load(std::memory_order_acquire);
        }

      private:
        const std::atomic<bool>& flag_;
    };

    std::atomic<bool> flag_{false};
    Token token_{flag_};
};

} // namespace fastecu::flash
