#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDensoSh72543CanDieselExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override;

    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
