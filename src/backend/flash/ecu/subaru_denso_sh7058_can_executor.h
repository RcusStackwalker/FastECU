#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh7058CanExecutor final : public FamilyExecutor<SubaruDensoSh7058CanPlan, ICanFlashExecutor>
{
  public:
    Status BeforeTransportConfigure(ICanFlashTransport& transport, IClock& clock,
                                    const ICancellationToken& cancellation) const override;
    Status BeforeTransportOpen(const ICancellationToken& cancellation) const override;

  private:
    Result<Iso15765Config> Setup(const SubaruDensoSh7058CanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruDensoSh7058CanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
