#pragma once

#include "src/backend/flash/guarded_can_flash_transport.h"

#include "src/backend/flash/ecu/subaru_hitachi_sh7058_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruHitachiSh7058CanExecutor final : public FamilyExecutor<SubaruHitachiSh7058CanPlan, ICanFlashExecutor>
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;

  private:
    Result<Iso15765Config> Setup(const SubaruHitachiSh7058CanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruHitachiSh7058CanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
