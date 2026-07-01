#pragma once
#include "protocol/ican_transport.h"
class SerialPortActions;
namespace cdbg {
// Adapts FastECU's SerialPortActions raw-CAN mode to ICanTransport. Matches
// the wire convention already used by
// modules/ecu/flash_ecu_subaru_denso_sh705x_densocan.cpp: each frame is 4
// big-endian CAN-id bytes followed by the payload.
class FastEcuCanTransport : public ICanTransport {
public:
    explicit FastEcuCanTransport(SerialPortActions *serial) : serial_(serial) {}
    int write(quint32 canId, const QByteArray &payload) override;
    QByteArray read(int timeoutMs, quint32 &outId) override;
private:
    SerialPortActions *serial_;
};
}
