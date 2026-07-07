#pragma once
#include "protocol/bytes.h"

#include <cstdint>

namespace cdbg
{

// Raw-CAN transport (not ISO-TP): every write/read is one CAN frame,
// addressed by an explicit 11-bit arbitration ID.
class ICanTransport
{
  public:
    virtual ~ICanTransport() = default;
    // Sends `payload` (up to 8 bytes) on CAN id `canId`. Returns bytes written.
    virtual int write(std::uint32_t canId, bytes::ByteView payload) = 0;
    // Reads one CAN frame within timeoutMs. Returns the frame's payload and
    // sets outId to its CAN id, or returns empty bytes if nothing
    // arrived in time.
    virtual bytes::Bytes read(int timeoutMs, std::uint32_t& outId) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};

} // namespace cdbg
