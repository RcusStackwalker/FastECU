#pragma once
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8104_can_plan.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/guarded_can_flash_transport.h"

namespace fastecu::flash
{
class SubaruTcuCvtMitsuMh8104CanExecutor final
    : public FamilyExecutor<SubaruTcuCvtMitsuMh8104CanPlan, ICanFlashExecutor>
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;

  private:
    Result<Iso15765Config> Setup(const SubaruTcuCvtMitsuMh8104CanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruTcuCvtMitsuMh8104CanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
