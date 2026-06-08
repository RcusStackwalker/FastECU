#pragma once
#include "protocol/ikline_transport.h"
class SerialPortActions;
namespace mutdma {
// Adapts FastECU's SerialPortActions to IKlineTransport.
class FastEcuKlineTransport : public IKlineTransport {
public:
    explicit FastEcuKlineTransport(SerialPortActions* serial) : serial_(serial) {}
    bool setBaud(int baud) override;
    int  write(const QByteArray& data) override;
    QByteArray read(int timeoutMs, int wantBytes = -1) override;
private:
    SerialPortActions* serial_;
};
}
