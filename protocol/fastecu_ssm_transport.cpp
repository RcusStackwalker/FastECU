#include "protocol/fastecu_ssm_transport.h"
#include "serial_port/serial_port_actions.h"

int FastEcuSsmTransport::write(const QByteArray &data)
{
    serial_->write_serial_data_echo_check(data);
    return data.size();
}

QByteArray FastEcuSsmTransport::read(int timeoutMs)
{
    return serial_->read_serial_data(static_cast<uint16_t>(timeoutMs));
}

bool FastEcuSsmTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}
