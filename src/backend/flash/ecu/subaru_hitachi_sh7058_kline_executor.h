#pragma once

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh7058_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruHitachiSh7058KlineExecutor final : public FamilyExecutor<SubaruHitachiSh7058KlinePlan, IKlineFlashExecutor>
{
  private:
    Result<KlineConfig> Setup(const SubaruHitachiSh7058KlinePlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruHitachiSh7058KlinePlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
