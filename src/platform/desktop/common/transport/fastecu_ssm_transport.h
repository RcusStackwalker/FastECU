#pragma once
#include "src/backend/protocol/issm_transport.h"
class SerialPortActions;

// Adapts FastECU's SerialPortActions to ISsmTransport.
class FastEcuSsmTransport : public ISsmTransport
{
  public:
    explicit FastEcuSsmTransport(SerialPortActions *serial) : serial_(serial)
    {
    }
    fastecu::Result<std::size_t> write(bytes::ByteView data) override;
    fastecu::Result<OptionalBytes> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    SerialPortActions *serial_;
};
