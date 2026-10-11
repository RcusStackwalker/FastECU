#pragma once
#include "src/backend/protocol/issm_transport.h"

#include <chrono>
class SerialPortActions;

// Adapts FastECU's SerialPortActions to fastecu::ISsmTransport.
class FastEcuSsmTransport : public fastecu::ISsmTransport
{
  public:
    explicit FastEcuSsmTransport(SerialPortActions *serial) : serial_(serial)
    {
    }
    fastecu::Result<std::size_t> Write(bytes::ByteView data) override;
    fastecu::Result<OptionalBytes> Read(std::chrono::milliseconds timeout,
                                        const fastecu::ICancellationToken& cancellation) override;
    bool IsOpen() const override;

  private:
    SerialPortActions *serial_;
};
