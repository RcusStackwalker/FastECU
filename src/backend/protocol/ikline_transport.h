#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <cstddef>
#include <optional>

namespace mutdma
{
class IKlineTransport
{
  public:
    using OptionalBytes = std::optional<bytes::Bytes>;

    virtual ~IKlineTransport() = default;
    virtual fastecu::Status setBaud(int baud) = 0;
    // The desktop serial facade exposes success, not a measured driver count;
    // its successful result is therefore the requested data size.
    virtual fastecu::Result<std::size_t> write(bytes::ByteView data) = 0;
    virtual fastecu::Result<OptionalBytes> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) = 0;
    // True if the underlying adapter connection is open. Used to distinguish
    // "adapter disconnected" from "ECU not responding" in LoggingProtocol wrappers.
    virtual bool isOpen() const = 0;
};
} // namespace mutdma
