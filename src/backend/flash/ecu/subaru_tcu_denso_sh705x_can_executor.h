#pragma once

#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

class SubaruTcuDensoSh705xCanExecutor final : public FamilyExecutor<SubaruTcuDensoSh705xCanPlan, ICanFlashExecutor>
{
  private:
    Result<Iso15765Config> Setup(const SubaruTcuDensoSh705xCanPlan& family) const override;
    Result<FlashExecutionResult> Run(const SubaruTcuDensoSh705xCanPlan& family, const FlashPlan& plan,
                                     ICanFlashTransport& transport, IClock& clock,
                                     const ICancellationToken& cancellation, IEventSink& events) override;
};

} // namespace fastecu::flash
