#pragma once

#include <optional>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/uds_client.h"

// Shared UdsClient exchange plumbing for this package's CAN executors
// (mitsu_colt_m32r, subaru_hitachi_m32r, subaru_tcu_cvt_hitachi_m32r,
// subaru_tcu_cvt_mitsu_mh8111 -- subaru_tcu_cvt_mitsu_mh8104 talks to
// uds::IUdsChannel directly and does not use this, see that executor's own
// file header). Extracted because all four families independently carried a
// byte-for-byte identical copy of this logging/wrapping logic.
namespace fastecu::flash
{

// Logs and returns `failure` unchanged.
//
// A rejection is the ECU/TCU's own answer: the request went out and came
// back refused (or the bus failed under it), and `failure.detail` says why --
// logged with `rejection_prefix` first. A cancellation is the operator's
// stop and says NOTHING about what the ECU did: UdsClient checks the
// cancellation token both before transmitting and while waiting for the
// reply, so a cancelled exchange may or may not have reached the ECU --
// calling that "rejected" would misinform an operator deciding whether to
// power-cycle the unit. Cancellation therefore gets its own operator-facing
// line instead. `operation` names what was being asked, phrased to read
// after "during": "the erase trigger", "TransferData to 0x8000".
Error report_exchange_failure(IEventSink& events, const Error& failure,
                              std::string_view rejection_prefix, std::string_view operation);

// The pieces every UdsClient-backed exchange below needs, bundled so call
// sites needing a non-default client (a second UdsClient bound to a
// different CAN id pair, for example) can build one inline instead of
// growing fatal_request's/non_fatal_query's own parameter list.
struct UdsExchangeContext
{
    uds::UdsClient& client;
    const uds::ExchangePolicy& policy;
    const ICancellationToken& cancellation;
    IEventSink& events;
};

// Sends `pdu` through `ctx.client` and, on failure, logs via
// report_exchange_failure with a fixed `rejection_prefix` and returns the
// error. The shape every fatal exchange uses except any whose expected
// reply does not follow the standard SID+0x40 convention UdsClient itself
// enforces (those go through uds::IUdsChannel directly).
Result<bytes::Bytes> fatal_request(const UdsExchangeContext& ctx, bytes::ByteView pdu,
                                   std::string_view rejection_prefix, std::string_view operation);

// Sends `pdu` through `ctx.client`; on a mismatch or exchange failure, logs
// and returns without halting the caller -- legacy's non-fatal identity-query
// blocks (ECU/TCU ID, VIN, CAL ID, CVN, ...) never return early on these.
// `label` names the field for the success log line ("ECU ID", "VIN", ...).
void non_fatal_query(const UdsExchangeContext& ctx, bytes::ByteView pdu,
                     std::optional<bytes::Byte> expected_subfunction,
                     std::string_view rejection_prefix, std::string_view label);

} // namespace fastecu::flash
