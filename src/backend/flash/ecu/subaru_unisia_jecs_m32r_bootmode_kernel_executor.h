#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Attempt 1 of a bootmode Write: uploads the padded kernel into the
// M32R boot ROM with VPP and MOD1 raised.
class SubaruUnisiaJecsM32rBootModeKernelExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> TransportSetup(const FlashPlan& plan) const override;
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
