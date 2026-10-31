#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash::testing
{

// Match BoundAttempt's explicit opt-in for callers that already own lifecycle.
template <typename Executor>
Result<FlashExecutionResult> ExecuteWithCancellationGuard(Executor& executor, const FlashPlan& plan,
                                                          typename Executor::TransportType& transport, IClock& clock,
                                                          const ICancellationToken& cancellation, IEventSink& events)
{
    if constexpr (requires { typename Executor::CancellationGuard; })
    {
        typename Executor::CancellationGuard guard(transport);
        return executor.Execute(plan, guard, clock, cancellation, events);
    }
    else
    {
        return executor.Execute(plan, transport, clock, cancellation, events);
    }
}

} // namespace fastecu::flash::testing
