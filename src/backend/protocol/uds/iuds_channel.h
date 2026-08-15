#pragma once

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <optional>

namespace uds
{

// One UDS application PDU in each direction.
//
// Implementations own the transport envelope -- the 4-byte big-endian CAN
// arbitration id for CanFlashUdsChannel, a KWP2000 0x80 header and trailing
// checksum for a future K-Line channel -- so UdsClient and every executor
// above it work purely in PDUs starting at the service id.
//
// Deliberately narrower than the transports it wraps: no configure, no
// open/close, no unblock. Connection lifetime stays with whoever owns the
// transport; this interface exists only for the duration of an exchange.
class IUdsChannel
{
  public:
    virtual ~IUdsChannel() = default;

    // Adds the envelope and transmits. `pdu` starts at the service id.
    virtual fastecu::Status send(bytes::ByteView pdu,
                                 const fastecu::ICancellationToken& cancellation) = 0;

    // Returns the next PDU with the envelope stripped, starting at the
    // response service id. A read that reaches its deadline with nothing
    // received is a successful empty optional; cancellation, disconnection,
    // and a frame that fails envelope validation are errors.
    virtual fastecu::Result<std::optional<bytes::Bytes>> receive(
        int timeout_ms, const fastecu::ICancellationToken& cancellation) = 0;
};

} // namespace uds
