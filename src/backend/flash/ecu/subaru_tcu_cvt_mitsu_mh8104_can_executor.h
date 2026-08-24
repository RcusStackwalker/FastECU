#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
class SubaruTcuCvtMitsuMh8104CanExecutor final : public ICanFlashExecutor
{
  public:
    Result<Iso15765Config> transport_setup(const FlashPlan& plan) const override;

    Result<FlashExecutionResult> execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;
};
} // namespace fastecu::flash
