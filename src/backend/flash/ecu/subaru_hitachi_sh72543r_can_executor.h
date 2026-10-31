#pragma once

#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_sh72543r_can_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruHitachiSh72543rCanExecutor final : public FamilyExecutor<SubaruHitachiSh72543rCanPlan, ICanFlashExecutor>
{
  private:
    Result<Iso15765Config> Setup(const SubaruHitachiSh72543rCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruHitachiSh72543rCanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
