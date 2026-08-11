#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Portable equivalent of FlashEcuMitsuM32rCanOperation (deleted by step 5
// tail wave 0). Preserves its exact wire bytes, chunk sizes, inter-exchange
// delays, response-validation rules, and log text.
//
// Two structural differences from the class it replaces, both forced by the
// dialog-free executor contract and both recorded in the flash qualification
// matrix:
//
//  - The protocol, MCU, exact ROM-capacity, and operation-support checks moved into
//    build_mitsu_colt_m32r_can_plan, so they run before any I/O.
//  - The two mid-operation QMessageBox gates (erase trigger, top-128KB
//    bootstrap) became declared ConfirmationSpecs that the desktop dialog
//    answers BEFORE execute() is called. Their presence on the plan means
//    "granted"; this executor never prompts.
//
// Read and Write both consume the capacity and authorization snapshotted in
// MitsuColtM32rCanPlan. TestWrite remains unsupported and is rejected before
// an executor is normally constructed, with a defensive executor guard too.
class MitsuColtM32rCanExecutor final : public IFlashExecutor
{
  public:
    Result<FlashExecutionResult> execute(const FlashPlan& plan, IFlashTransport& transport,
                                         IClock& clock, const ICancellationToken& cancellation,
                                         IEventSink& events) override;
};

} // namespace fastecu::flash
