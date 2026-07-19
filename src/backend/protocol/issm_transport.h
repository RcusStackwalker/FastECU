#pragma once
#include "src/algorithms/protocol/bytes.h"

// Raw byte-stream transport for the SSM logging protocol (request/response over
// K-Line, ISO14230, or CAN depending on vehicle configuration -- the wire framing
// differences are handled by SerialPortActions itself, not by this seam).
class ISsmTransport
{
  public:
    virtual ~ISsmTransport() = default;
    // Returns bytes written.
    virtual int write(bytes::ByteView data) = 0;
    // Reads whatever is available within timeoutMs. Returns empty bytes
    // if nothing arrived in time.
    virtual bytes::Bytes read(int timeoutMs) = 0;
    // True if the underlying adapter connection is open.
    virtual bool isOpen() const = 0;
};
