#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Attempt 2 of a bootmode Write: erases and programs through the
// kernel attempt 1 uploaded, with VPP raised and MOD1 dropped.
class SubaruUnisiaJecsM32rBootModeProgramExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& plan) const override;
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
