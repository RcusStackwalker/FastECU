#pragma once

#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruTcuHitachiM32rKlineExecutor final
    : public FamilyExecutor<SubaruTcuHitachiM32rKlinePlan, IKlineFlashExecutor>
{
  private:
    Result<KlineConfig> Setup(const SubaruTcuHitachiM32rKlinePlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruTcuHitachiM32rKlinePlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
