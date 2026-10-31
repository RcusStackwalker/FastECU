#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/guarded_can_flash_transport.h"
#include "src/backend/flash/guarded_mixed_can_flash_transport.h"

namespace fastecu::flash
{

Result<FlashExecutionResult> ExecuteWithTransportGuard(ICanFlashExecutor& executor, const FlashPlan& plan,
                                                       ICanFlashTransport& transport, IClock& clock,
                                                       const ICancellationToken& cancellation, IEventSink& events)
{
    GuardedCanFlashTransport guard(transport);
    return executor.Execute(plan, guard, clock, cancellation, events);
}

Result<FlashExecutionResult> ExecuteWithTransportGuard(IMixedCanFlashExecutor& executor, const FlashPlan& plan,
                                                       IMixedCanFlashTransport& transport, IClock& clock,
                                                       const ICancellationToken& cancellation, IEventSink& events)
{
    GuardedMixedCanFlashTransport guard(transport);
    return executor.Execute(plan, guard, clock, cancellation, events);
}

Result<FlashExecutionResult> ExecuteWithTransportGuard(IKlineFlashExecutor& executor, const FlashPlan& plan,
                                                       IKlineFlashTransport& transport, IClock& clock,
                                                       const ICancellationToken& cancellation, IEventSink& events)
{
    return executor.Execute(plan, transport, clock, cancellation, events);
}

} // namespace fastecu::flash
