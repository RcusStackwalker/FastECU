#pragma once

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// Attempt 2 of a bootmode Write: erases and programs through the
// kernel attempt 1 uploaded, with VPP raised and MOD1 dropped.
class SubaruUnisiaJecsM32rBootModeProgramExecutor final
    : public FamilyExecutor<SubaruUnisiaJecsM32rBootModeProgramPlan, IKlineFlashExecutor>
{
  public:
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;

  private:
    Result<KlineConfig> Setup(const SubaruUnisiaJecsM32rBootModeProgramPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruUnisiaJecsM32rBootModeProgramPlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
