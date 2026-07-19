#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

int FastEcuSsmTransport::write(bytes::ByteView data)
{
    serial_->write_serial_data_echo_check(bytes::toQByteArray(data));
    return static_cast<int>(data.size());
}

bytes::Bytes FastEcuSsmTransport::read(int timeoutMs)
{
    return bytes::fromQByteArray(serial_->read_serial_data(static_cast<uint16_t>(timeoutMs)));
}

bool FastEcuSsmTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
