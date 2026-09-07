#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh705x_densocan_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruDensoSh705xDensoCanExecutor final : public IMixedCanFlashExecutor
{
  public:
    Result<MixedCanConfig> transport_setup(const FlashPlan& plan) const override;
    Status before_transport_open(const ICancellationToken& cancellation) const override;
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IMixedCanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
