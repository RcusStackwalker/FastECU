#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruMitsuM32rKlineExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
