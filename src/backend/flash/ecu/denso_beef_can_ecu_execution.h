#pragma once

#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// The ECU operation order for plans admitted by the family executors.
// Overloads admit only the two ECU family types; collaborators remain caller-owned.
Result<FlashExecutionResult> RunDensoBeefCanEcu(const SubaruDensoSh7058CanPlan& family, const FlashPlan& plan,
                                                ICanFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events);

Result<FlashExecutionResult> RunDensoBeefCanEcu(const SubaruDensoSh7058CanDieselPlan& family, const FlashPlan& plan,
                                                ICanFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events);

} // namespace fastecu::flash
