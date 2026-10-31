#pragma once

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDensoMc68hc16y5_02BdmExecutor final
    : public FamilyExecutor<SubaruDensoMc68hc16y5_02BdmPlan, IKlineFlashExecutor>
{
  public:
    Status BeforeTransportConfigure(IKlineFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;

  private:
    Result<KlineConfig> Setup(const SubaruDensoMc68hc16y5_02BdmPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruDensoMc68hc16y5_02BdmPlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
