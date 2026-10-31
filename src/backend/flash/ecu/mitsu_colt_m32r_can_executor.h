#pragma once
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/guarded_can_flash_transport.h"

namespace fastecu::flash
{

// Portable Mitsubishi Colt M32R CAN executor.
// Direct callers supply a GuardedCanFlashTransport; BoundAttempt installs it.
class MitsuColtM32rCanExecutor final : public FamilyExecutor<MitsuColtM32rCanPlan, ICanFlashExecutor>
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;

  private:
    Result<Iso15765Config> Setup(const MitsuColtM32rCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const MitsuColtM32rCanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
