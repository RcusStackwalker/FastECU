#pragma once
#include "protocol/issm_transport.h"
class SerialPortActions;

// Adapts FastECU's SerialPortActions to ISsmTransport.
class FastEcuSsmTransport : public ISsmTransport
{
  public:
    explicit FastEcuSsmTransport(SerialPortActions *serial) : serial_(serial)
    {
    }
    int write(bytes::ByteView data) override;
    bytes::Bytes read(int timeoutMs) override;
    bool isOpen() const override;

  private:
    SerialPortActions *serial_;
};
