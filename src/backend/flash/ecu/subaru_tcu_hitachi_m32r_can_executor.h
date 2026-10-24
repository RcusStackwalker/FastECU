#pragma once

#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_can_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruTcuHitachiM32rCanExecutor final : public FamilyExecutor<SubaruTcuHitachiM32rCanPlan, ICanFlashExecutor>
{
  private:
    Result<Iso15765Config> Setup(const SubaruTcuHitachiM32rCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruTcuHitachiM32rCanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
