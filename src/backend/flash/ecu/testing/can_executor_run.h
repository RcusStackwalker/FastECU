#pragma once

#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/manual_cancellation_token.h"
#include "src/backend/ports/testing/fake_clock.h"
#include "src/backend/ports/testing/recording_event_sink.h"

namespace fastecu::flash::testing
{

// One Execute() of a default-constructed `Executor` against a scripted
// transport, with the collaborators it ran against. Members are public and in
// declaration order so a test can bind them:
//   const auto [clock, events, cancellation, result] = ExecutorRun<E>(*plan, transport);
// The caller asserts the plan built before dereferencing it (see
// can_executor_conformance.h).
template <class Executor, class Clock = FakeClock> struct ExecutorRun
{
    Clock clock;
    RecordingEventSink events;
    ManualCancellationToken cancellation;
    Result<FlashExecutionResult> result;

    ExecutorRun(const FlashPlan& plan, ICanFlashTransport& transport)
        : result(Executor{}.Execute(plan, transport, clock, cancellation, events))
    {
    }
};

} // namespace fastecu::flash::testing
