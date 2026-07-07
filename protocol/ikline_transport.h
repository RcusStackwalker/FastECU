#pragma once
#include "protocol/bytes.h"

namespace mutdma {
class IKlineTransport {
public:
    virtual ~IKlineTransport() = default;
    virtual bool setBaud(int baud) = 0;
    virtual int write(bytes::ByteView data) = 0;          // returns bytes written
    virtual bytes::Bytes read(int timeoutMs, int wantBytes = -1) = 0;
    // True if the underlying adapter connection is open. Used to distinguish
    // "adapter disconnected" from "ECU not responding" in LoggingProtocol wrappers.
    virtual bool isOpen() const = 0;
};
}
