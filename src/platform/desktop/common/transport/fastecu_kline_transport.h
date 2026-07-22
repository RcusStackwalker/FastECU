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
    fastecu::Status setBaud(int baud) override;
    fastecu::Result<std::size_t> write(bytes::ByteView data) override;
    fastecu::Result<OptionalBytes> read(
        int timeoutMs, const fastecu::ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    SerialPortActions *serial_;
};
} // namespace mutdma
