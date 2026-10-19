#pragma once
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/guarded_can_flash_transport.h"

namespace fastecu::flash
{

// Portable Mitsubishi Colt M32R CAN executor.
class MitsuColtM32rCanExecutor final : public ICanFlashExecutor
{
  public:
    using CancellationGuard = GuardedCanFlashTransport;

    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override;

    // Direct callers supply a GuardedCanFlashTransport; BoundAttempt installs it.
    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
