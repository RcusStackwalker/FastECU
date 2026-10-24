#pragma once
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruTcuCvtMitsuMh8111CanExecutor final
    : public FamilyExecutor<SubaruTcuCvtMitsuMh8111CanPlan, ICanFlashExecutor>
{
  private:
    Result<Iso15765Config> Setup(const SubaruTcuCvtMitsuMh8111CanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruTcuCvtMitsuMh8111CanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
