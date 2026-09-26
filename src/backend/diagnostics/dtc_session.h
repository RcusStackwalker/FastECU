#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/diagnostics/obd_frames.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/idiagnostic_link.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fastecu::diagnostics
{

enum class DtcOperation
{
    Read,
    Clear,
};

struct DtcRequest
{
    ObdProtocol protocol = ObdProtocol::Iso9141;
    DtcOperation operation = DtcOperation::Read;
};

struct SupportedPidPage
{
    std::size_t page = 0;
    bytes::Bytes bitmap;
};

struct DtcReport
{
    std::vector<SupportedPidPage> supported_pids;
    std::optional<bytes::Bytes> monitor_status, vin_length, vin, cal_id_length, cal_id, cvn_length, cvn;
    std::vector<std::uint16_t> stored, pending;
    bool cleared = false;
};

// One OBD-II DTC read or clear, reproducing the legacy DtcOperations dialog's
// wire sequence, sleeps, and log wording (step 6g spec, "DTC today"). Always
// clears the link's header and resets it before returning.
Result<DtcReport> run_dtc_session(const DtcRequest& request, IDiagnosticLink& link, IClock& clock,
                                  const ICancellationToken& cancellation, IEventSink& events);

} // namespace fastecu::diagnostics
