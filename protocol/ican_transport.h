#pragma once
#include <QByteArray>

namespace cdbg {

// Raw-CAN transport (not ISO-TP): every write/read is one CAN frame,
// addressed by an explicit 11-bit arbitration ID.
class ICanTransport {
public:
    virtual ~ICanTransport() = default;
    // Sends `payload` (up to 8 bytes) on CAN id `canId`. Returns bytes written.
    virtual int write(quint32 canId, const QByteArray &payload) = 0;
    // Reads one CAN frame within timeoutMs. Returns the frame's payload and
    // sets outId to its CAN id, or returns an empty QByteArray if nothing
    // arrived in time.
    virtual QByteArray read(int timeoutMs, quint32 &outId) = 0;
};

} // namespace cdbg
