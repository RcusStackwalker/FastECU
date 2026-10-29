#include "src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h"

#include "src/backend/flash/ecu/denso_beef_can_ecu_execution.h"

namespace fastecu::flash
{

Result<Iso15765Config> SubaruDensoSh7058CanExecutor::Setup(const SubaruDensoSh7058CanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Status SubaruDensoSh7058CanExecutor::BeforeTransportConfigure(ICanFlashTransport& transport, IClock&,
                                                              const ICancellationToken& cancellation) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before petrol CAN reset");
    }
    if (const Status reset = transport.ResetConnection(); !reset)
    {
        return reset;
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after petrol CAN reset");
    }
    return {};
}

Status SubaruDensoSh7058CanExecutor::BeforeTransportOpen(const ICancellationToken& cancellation) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after petrol CAN configuration");
    }
    return {};
}

Result<FlashExecutionResult> SubaruDensoSh7058CanExecutor::Run(const SubaruDensoSh7058CanPlan& family,
                                                               const FlashPlan& plan, ICanFlashTransport& transport,
                                                               IClock& clock, const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    return RunDensoBeefCanEcu(family, plan, transport, clock, cancellation, events);
}

} // namespace fastecu::flash
