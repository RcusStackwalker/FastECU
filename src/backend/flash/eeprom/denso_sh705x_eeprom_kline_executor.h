#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable equivalent of the legacy EepromEcuSubaruDensoSH705xKlineOperation
// (deleted by this task). Preserves its exact wire bytes, chunk sizes,
// timeouts, and EEPROM-mode framing; the confirm()/dialog loop moves to the
// desktop orchestrator (Task 17) since a synchronous executor call never
// blocks for a human answer -- this executor performs exactly one bootstrap
// + read for the single EepromReadMode carried by the plan.
class DensoSh705xEepromKlineExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, IEventSink& events) override;

  private:
    // Note: unlike the brief's illustrative sketch, upload_kernel() also
    // takes the family plan (not just the KernelImage) -- it needs
    // kline_plan.tester_id/target_id for its SSM-framed exchanges, the same
    // as connect_bootloader(). Both values are family constants (0xf0/0x10)
    // today, but reading them from the plan rather than hardcoding keeps the
    // two functions symmetric and avoids a second source of truth.
    Status connect_bootloader(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const DensoSh705xEepromKlinePlan& kline_plan, bool& kernel_alive);
    Status upload_kernel(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                         IEventSink& events, const DensoSh705xEepromKlinePlan& kline_plan, const KernelImage& kernel);
    Result<bytes::Bytes> read_mem(IKlineFlashTransport& transport, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events,
                                  const MemoryRegion& region, EepromReadMode mode);
};

} // namespace fastecu::flash
