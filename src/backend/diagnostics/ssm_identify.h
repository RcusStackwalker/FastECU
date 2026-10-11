#pragma once
#include <optional>
#include <string>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/idiagnostic_link.h"

namespace fastecu::diagnostics
{

// Which legacy identification exchange to run. The toolbar's log transport
// picks it: "SSM" -> Ssm1, "K-Line" -> KlineSsm2, "iso15765" -> Iso15765Uds.
// Raw "CAN" never identified an ECU and has no variant.
enum class SsmVariant
{
    kSsm1,
    kKlineSsm2,
    kIso15765Uds,
};

// The toolbar's ECU/TCU radio button.
enum class SsmTarget
{
    kEcu,
    kTcu,
};

struct SsmIdentifyRequest
{
    SsmVariant variant = SsmVariant::kKlineSsm2;
    SsmTarget target = SsmTarget::kEcu;
};

struct SsmIdentity
{
    std::string ecu_id;         // uppercase hex, no separators
    bytes::Bytes init_response; // the init frame; empty for Iso15765Uds
};

// One identification attempt. Opens the link itself, sends the variant's
// request, and checks the answer; retry policy belongs to the caller.
// Timeout: nothing came back. BadResponse: something came back but is short
// or fails validation. Cancelled: the token tripped in a read or a sleep.
// Link errors pass through unchanged. Logs nothing.
Result<SsmIdentity> IdentifySsmEcu(IDiagnosticLink& link, IClock& clock, const ICancellationToken& cancellation,
                                   const SsmIdentifyRequest& request);

// 80 {10|18} F0 len payload... checksum, the checksum being the 8-bit sum of
// every byte before it.
bytes::Bytes SsmFrame(bytes::ByteView payload, SsmTarget target);

// The five ECU-ID bytes at offset 8 as uppercase hex, or nullopt when the
// frame is too short to hold them.
std::optional<std::string> ParseSsmEcuId(bytes::ByteView init_response);

} // namespace fastecu::diagnostics
