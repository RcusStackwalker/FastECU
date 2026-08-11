#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable Mitsubishi Colt M32R CAN executor.
class MitsuColtM32rCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;
};

} // namespace fastecu::flash
