#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/uds/uds_pdu.h"

#include <optional>
#include <string>

namespace uds
{

enum class ResponseKind
{
    Positive,
    Negative,
    Malformed,
};

struct Response
{
    ResponseKind kind{ResponseKind::Malformed};

    // Always the *request* SID: a 0x67 positive response and a 7F 27 xx
    // negative response both report 0x27, so callers compare against the
    // service they sent without doing offset arithmetic.
    bytes::Byte service{};

    // Meaningful only when kind == Negative.
    bytes::Byte nrc{};

    // What the response carries past its own header: the bytes after the
    // service id for a Positive response, the bytes after the NRC for a
    // Negative one (7F <sid> <nrc>, so subspan(3) -- past the NRC, not past
    // the SID), and empty for a Malformed one. A VIEW INTO THE INPUT, not a
    // copy: it stays valid only as long as the buffer passed to parseResponse.
    //
    // Nothing in production reads this field. UdsClient classifies with kind,
    // isPending() and matches(), and hands executors an owning Bytes they read
    // with payload()/subfunction() below. It is part of the parse result the
    // layer's design specifies and is pinned by uds_response_test, which is
    // its only reader; a first production reader has to honour the lifetime
    // rule above.
    bytes::ByteView data;

    bool isPending() const
    {
        return kind == ResponseKind::Negative && nrc == kNrcResponsePending;
    }

    bool matches(bytes::Byte sid) const
    {
        return kind == ResponseKind::Positive && service == sid;
    }
};

// Classification is three rules applied in order:
//
//   1. pdu[0] == 0x7F is Negative, and requires at least three bytes
//      (7F <sid> <nrc>); shorter is Malformed.
//   2. pdu[0] >= 0x40 is Positive, with service = pdu[0] - 0x40.
//   3. Anything else, including an empty PDU, is Malformed.
//
// Rule 1 cannot collide with a legitimate positive response: that would
// require request SID 0x3F, which UDS reserves and no family uses.
Response parseResponse(bytes::ByteView pdu);

// Everything after the service id. Empty for an empty or service-only PDU.
bytes::ByteView payload(bytes::ByteView pdu);

// The byte after the service id, when the PDU has one.
std::optional<bytes::Byte> subfunction(bytes::ByteView pdu);

// Human-readable text for a negative-response frame, delegating to the shared
// NRC table in //src/algorithms/diagnostics. The table is not duplicated here.
std::string describe(bytes::ByteView pdu);

} // namespace uds
