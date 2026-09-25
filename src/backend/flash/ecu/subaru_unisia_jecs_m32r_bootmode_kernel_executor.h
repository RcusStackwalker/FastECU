#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Wave 7, attempt 1 of a bootmode Write: uploads the padded kernel into the
// M32R boot ROM with VPP and MOD1 raised.
class SubaruUnisiaJecsM32rBootModeKernelExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
