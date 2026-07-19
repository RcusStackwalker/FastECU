#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"
namespace mutdma
{
bool FastEcuKlineTransport::setBaud(int baud)
{
    return serial_->change_port_speed(QString::number(baud)) == 0;
}
int FastEcuKlineTransport::write(bytes::ByteView data)
{
    serial_->write_serial_data(bytes::toQByteArray(data));
    return static_cast<int>(data.size());
}
bytes::Bytes FastEcuKlineTransport::read(int timeoutMs, int /*wantBytes*/)
{
    return bytes::fromQByteArray(serial_->read_serial_data(quint16(timeoutMs)));
}
bool FastEcuKlineTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
} // namespace mutdma
