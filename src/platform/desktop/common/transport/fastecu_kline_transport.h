#pragma once
#include "src/backend/protocol/ikline_transport.h"
class SerialPortActions;
namespace mutdma
{
// Adapts FastECU's SerialPortActions to IKlineTransport.
class FastEcuKlineTransport : public IKlineTransport
{
  public:
    explicit FastEcuKlineTransport(SerialPortActions *serial) : serial_(serial)
    {
    }
    fastecu::Status SetBaud(int baud) override;
    fastecu::Result<std::size_t> Write(bytes::ByteView data) override;
    fastecu::Result<OptionalBytes> Read(std::chrono::milliseconds timeout,
                                        const fastecu::ICancellationToken& cancellation) override;
    bool IsOpen() const override;

  private:
    SerialPortActions *serial_;
};
} // namespace mutdma
