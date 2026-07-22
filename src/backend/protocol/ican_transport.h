#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace cdbg
{

struct CanFrame
{
    std::uint32_t id;
    bytes::Bytes payload;
};

// Raw-CAN transport (not ISO-TP): every write/read is one CAN frame,
// addressed by an explicit 11-bit arbitration ID.
class ICanTransport
{
  public:
    virtual ~ICanTransport() = default;
    // Sends `payload` (up to 8 bytes) on CAN id `canId`. Returns bytes written.
    virtual fastecu::Result<std::size_t> write(
        std::uint32_t canId, bytes::ByteView payload) = 0;
    // A normal logging deadline is a successful empty optional. Cancellation,
    // disconnection, and driver failures are errors.
    virtual fastecu::Result<std::optional<CanFrame>> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};

} // namespace cdbg
