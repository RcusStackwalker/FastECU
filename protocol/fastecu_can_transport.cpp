#include "protocol/fastecu_can_transport.h"
#include "serial_port/serial_port_actions.h"

namespace cdbg {

int FastEcuCanTransport::write(quint32 canId, const QByteArray &payload)
{
    QByteArray frame;
    frame.append(char((canId >> 24) & 0xFF));
    frame.append(char((canId >> 16) & 0xFF));
    frame.append(char((canId >> 8) & 0xFF));
    frame.append(char(canId & 0xFF));
    frame.append(payload);
    serial_->write_serial_data_echo_check(frame);
    return payload.size();
}

QByteArray FastEcuCanTransport::read(int timeoutMs, quint32 &outId)
{
    QByteArray raw = serial_->read_serial_data(quint16(timeoutMs));
    if (raw.size() < 4)
        return QByteArray();
    outId = (quint32(quint8(raw.at(0))) << 24) | (quint32(quint8(raw.at(1))) << 16)
          | (quint32(quint8(raw.at(2))) << 8)  |  quint32(quint8(raw.at(3)));
    return raw.mid(4);
}

bool FastEcuCanTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}

}
