#include "protocol/fastecu_kline_transport.h"
#include "serial_port/serial_port_actions.h"
namespace mutdma {
bool FastEcuKlineTransport::setBaud(int baud) {
    return serial_->change_port_speed(QString::number(baud)) == 0;
}
int FastEcuKlineTransport::write(const QByteArray& data) {
    serial_->write_serial_data(data);
    return data.size();
}
QByteArray FastEcuKlineTransport::read(int timeoutMs, int /*wantBytes*/) {
    return serial_->read_serial_data(quint16(timeoutMs));
}
}
