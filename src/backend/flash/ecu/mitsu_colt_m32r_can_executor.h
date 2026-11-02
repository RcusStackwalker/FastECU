#pragma once
#include "src/backend/flash/ecu/mitsu_colt_m32r_can_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable Mitsubishi Colt M32R CAN executor.
// BoundAttempt installs the production GuardedCanFlashTransport.
class MitsuColtM32rCanExecutor final : public FamilyExecutor<MitsuColtM32rCanPlan, ICanFlashExecutor>
{
  private:
    Result<Iso15765Config> Setup(const MitsuColtM32rCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const MitsuColtM32rCanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
