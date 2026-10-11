#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh7058CanExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> TransportSetup(const FlashPlan& plan) const override;
    Status BeforeTransportConfigure(ICanFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;
    Status BeforeTransportOpen(const ICancellationToken& cancellation) const override;

    Result<FlashExecutionResult> Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
