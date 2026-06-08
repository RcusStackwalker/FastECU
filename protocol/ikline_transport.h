#pragma once
#include <QByteArray>
namespace mutdma {
class IKlineTransport {
public:
    virtual ~IKlineTransport() = default;
    virtual bool setBaud(int baud) = 0;
    virtual int  write(const QByteArray& data) = 0;          // returns bytes written
    virtual QByteArray read(int timeoutMs, int wantBytes = -1) = 0;
};
}
