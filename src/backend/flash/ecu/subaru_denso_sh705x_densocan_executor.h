#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/guarded_mixed_can_flash_transport.h"

namespace fastecu::flash
{

class SubaruDensoSh705xDensoCanExecutor final
    : public FamilyExecutor<SubaruDensoSh705xDensoCanPlan, IMixedCanFlashExecutor>
{
  public:
    using CancellationGuard = GuardedMixedCanFlashTransport;

    Status BeforeTransportConfigure(IMixedCanFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;
    Status BeforeTransportOpen(const ICancellationToken& cancellation) const override;

  private:
    Result<MixedCanConfig> Setup(const SubaruDensoSh705xDensoCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruDensoSh705xDensoCanPlan& family, const FlashPlan& plan,
                                     IMixedCanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
