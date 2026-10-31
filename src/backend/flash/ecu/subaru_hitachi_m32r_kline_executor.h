#pragma once

#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruHitachiM32rKlineExecutor final : public FamilyExecutor<SubaruHitachiM32rKlinePlan, IKlineFlashExecutor>
{
  private:
    Result<KlineConfig> Setup(const SubaruHitachiM32rKlinePlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruHitachiM32rKlinePlan& family, const FlashPlan& plan,
                                     IKlineFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
