#pragma once

#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/issm_transport.h"
#include "src/backend/service_functions/service_function_types.h"

namespace fastecu::service_functions
{

// Operator-gated, non-flash TCU routine. The platform owns the thread and the
// dialog; this runs bounded, cancellable I/O and yields when it needs a human.
//
// Deliberately NOT an IFlashExecutor: ConfirmationSpec requires every operator
// answer to be collected before execution begins, and relearn issues an
// instruction mid-sequence (legacy :735). See the design doc.
class ServiceFunctionSession
{
  public:
    virtual ~ServiceFunctionSession() = default;

    // Pure: validates the request and returns the transport configuration this
    // session requires. Performs no I/O, so an unusable request is rejected
    // before the caller touches hardware.
    virtual Result<SsmTransportConfig> transport_setup() const = 0;

    // Runs I/O until the next operator gate, completion, or failure. After a
    // GateStep the caller must submit() an answer before calling resume()
    // again; calling it with a gate outstanding is ErrorKind::Internal.
    virtual ServiceFunctionStep resume(ISsmTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                       IEventSink& events) = 0;

    virtual void submit(GateResponse response) = 0;
};

} // namespace fastecu::service_functions
