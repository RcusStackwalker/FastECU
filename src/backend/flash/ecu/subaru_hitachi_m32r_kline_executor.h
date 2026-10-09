#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruHitachiM32rKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
