#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruDenso1n83m_1_5mCanExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> transport_setup(const FlashPlan& plan) const override;

    Result<FlashExecutionResult> execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
