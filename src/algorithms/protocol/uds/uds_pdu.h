#pragma once

#include "src/algorithms/protocol/bytes.h"

// UDS/KWP2000 application-layer PDUs: [SID][subfunction?][data...].
//
// Pure and transport-independent. Nothing here knows about CAN arbitration
// ids, K-Line headers, timeouts, or retries -- the transport envelope belongs
// to IUdsChannel implementations (the interface and UdsClient are in
// //src/backend/protocol/uds; the CAN flash one is CanFlashUdsChannel in
// //src/backend/flash), and the exchange belongs to UdsClient.
//
// The model is dialect-neutral: UDS and KWP2000 share this PDU shape, this
// positive-response convention, and this negative-response frame. Only the
// concrete service set differs, and that lives with each family.
namespace uds
{

// A negative response is 7F <request SID> <NRC>.
inline constexpr bytes::Byte kNegativeResponse = 0x7F;

// A positive response echoes the request SID with this offset added.
inline constexpr bytes::Byte kPositiveResponseOffset = 0x40;

// requestCorrectlyReceived-ResponsePending. The ECU has accepted the request
// and needs more time. The correct reaction is to keep LISTENING; re-sending
// is wrong. UdsClient absorbs this automatically.
inline constexpr bytes::Byte kNrcResponsePending = 0x78;

// busyRepeatRequest. Deliberately NOT absorbed by UdsClient: honoring it means
// re-TRANSMITTING the request, which is unsafe for the non-idempotent services
// this layer's first users send -- RequestDownload, TransferData, and erase
// routines. Exported so a caller that knows its own service is idempotent can
// implement the retry at its own level, where the safety argument is visible
// in review.
inline constexpr bytes::Byte kNrcBusyRepeatRequest = 0x21;

constexpr bytes::Byte positiveResponse(bytes::Byte sid)
{
    return static_cast<bytes::Byte>(sid + kPositiveResponseOffset);
}

constexpr bytes::Byte requestFromPositive(bytes::Byte positive_sid)
{
    return static_cast<bytes::Byte>(positive_sid - kPositiveResponseOffset);
}

// Four overloads rather than one variadic: the subfunction-versus-first-data-
// byte distinction is exactly what gets mis-read at call sites, so it gets its
// own named parameter position. `buildRequest(kServiceSecurityAccess, 0x05_b)`
// is then unambiguous on sight.
//
// A family builder may compose its own frames with bytes::composeBe
// or delegate to these -- MitsuColtCan's builders delegate, which is what puts
// the [SID][...] shape in one place instead of one per service.
bytes::Bytes buildRequest(bytes::Byte sid);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::ByteView data);
bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data);

} // namespace uds
