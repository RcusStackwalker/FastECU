#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDensoSh72543CanDieselExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
