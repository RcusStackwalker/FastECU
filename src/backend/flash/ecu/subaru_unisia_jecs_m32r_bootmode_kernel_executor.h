#pragma once

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Attempt 1 of a bootmode Write: uploads the padded kernel into the
// M32R boot ROM with VPP and MOD1 raised.
class SubaruUnisiaJecsM32rBootModeKernelExecutor final
    : public FamilyExecutor<SubaruUnisiaJecsM32rBootModeKernelPlan, IKlineFlashExecutor>
{
  public:
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;

  private:
    Result<KlineConfig> Setup(const SubaruUnisiaJecsM32rBootModeKernelPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruUnisiaJecsM32rBootModeKernelPlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
