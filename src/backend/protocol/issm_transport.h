#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <cstddef>
#include <optional>

// Raw byte-stream transport for the SSM logging protocol (request/response over
// K-Line, ISO14230, or CAN depending on vehicle configuration -- the wire framing
// differences are handled by SerialPortActions itself, not by this seam).
class ISsmTransport
{
  public:
    using OptionalBytes = std::optional<bytes::Bytes>;

    virtual ~ISsmTransport() = default;
    virtual fastecu::Result<std::size_t> write(bytes::ByteView data) = 0;
    // A normal logging deadline is a successful empty optional. Cancellation,
    // disconnection, and driver failures are errors.
    virtual fastecu::Result<OptionalBytes> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};
