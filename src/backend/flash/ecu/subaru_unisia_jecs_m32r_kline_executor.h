#pragma once

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruUnisiaJecsM32rKlineExecutor final
    : public FamilyExecutor<SubaruUnisiaJecsM32rKlinePlan, IKlineFlashExecutor>
{
  public:
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;

  private:
    Result<KlineConfig> Setup(const SubaruUnisiaJecsM32rKlinePlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruUnisiaJecsM32rKlinePlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
