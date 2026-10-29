#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h"

#include <chrono>

#include "src/backend/flash/ecu/denso_beef_can_ecu_execution.h"

namespace fastecu::flash
{

using namespace std::chrono_literals;

Result<Iso15765Config> SubaruDensoSh7058CanDieselExecutor::Setup(const SubaruDensoSh7058CanDieselPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Status SubaruDensoSh7058CanDieselExecutor::BeforeTransportConfigure(ICanFlashTransport& transport, IClock& clock,
                                                                    const ICancellationToken& cancellation) const
{
    // The legacy execute() sequence (revision 59f4e442:64-78) resets the
    // serial connection, waits exactly 500 ms, then applies the ISO-15765
    // setters and opens the port.
    // BoundAttempt owns the lifecycle; this hook preserves the family-specific
    // pre-config ordering without exposing desktop serial details here.
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before CAN reset");
    }
    if (const Status reset = transport.ResetConnection(); !reset.has_value())
    {
        return reset;
    }
    return clock.Sleep(500ms, cancellation);
}

Result<FlashExecutionResult> SubaruDensoSh7058CanDieselExecutor::Run(const SubaruDensoSh7058CanDieselPlan& family,
                                                                     const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    return RunDensoBeefCanEcu(family, plan, transport, clock, cancellation, events);
}

} // namespace fastecu::flash
