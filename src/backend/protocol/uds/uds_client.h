#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/iuds_channel.h"

namespace uds
{

// Per-exchange timing. Vendor sequencing quirks live at the call site; only
// the timing knobs each exchange genuinely needs are parameters here.
struct ExchangePolicy
{
    // Quiet period between the write and the first read. Several families
    // need one; 0 skips the sleep entirely.
    int pre_read_delay_ms = 0;

    int read_timeout_ms = 500;

    // Read timeout used once the ECU has reported responsePending. Separate
    // from read_timeout_ms because "busy, wait" legitimately takes much
    // longer than a normal reply.
    int pending_timeout_ms = 3000;

    // Guard against an ECU that reports responsePending forever.
    int max_pending_repeats = 10;
};

// One UDS request/response round trip.
//
// Owns exactly one concern: getting a validated positive response back, or a
// typed error. It does not own session state, tester-present keepalive, or
// service sequencing -- those stay with each family's executor, where the
// vendor quirks are readable.
class UdsClient
{
  public:
    UdsClient(IUdsChannel& channel, fastecu::IClock& clock, fastecu::IEventSink& events);

    // Sends `pdu` and returns the positive-response PDU, service byte
    // included and envelope already stripped by the channel.
    //
    // NRC 0x78 (responsePending) is absorbed by re-READING, never by
    // re-sending. NRC 0x21 (busyRepeatRequest) is deliberately NOT absorbed:
    // honoring it means re-transmitting, which is unsafe for the
    // non-idempotent services this layer's callers send. It surfaces as an
    // ordinary negative response so the caller decides.
    fastecu::Result<bytes::Bytes> request(bytes::ByteView pdu, const ExchangePolicy& policy,
                                          const fastecu::ICancellationToken& cancellation);

  private:
    IUdsChannel& channel_;
    fastecu::IClock& clock_;
    fastecu::IEventSink& events_;
};

} // namespace uds
