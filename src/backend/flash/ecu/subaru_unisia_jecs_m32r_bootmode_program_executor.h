#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Wave 7, attempt 2 of a bootmode Write: erases and programs through the
// kernel attempt 1 uploaded, with VPP raised and MOD1 dropped.
class SubaruUnisiaJecsM32rBootModeProgramExecutor final : public IKlineFlashExecutor
{
  public:
    Result<KlineConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_configure(IKlineFlashTransport& transport, IClock& clock,
                                      const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
