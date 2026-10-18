#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable Mitsubishi Colt M32R CAN executor.
class MitsuColtM32rCanExecutor final : public ICanFlashExecutor
{
  public:
    // Opts this executor into the guarded transport pilot (ADR 0025):
    // BoundAttempt hands Execute a GuardedCanFlashTransport. Deleted once
    // every CAN family runs behind the guard.
    static constexpr bool kRunsBehindGuardedTransport = true;

    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override;

    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
