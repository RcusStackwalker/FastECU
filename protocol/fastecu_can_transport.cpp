#include "protocol/fastecu_can_transport.h"
#include "protocol/qt_bytes.h"
#include "serial_port/serial_port_actions.h"

namespace cdbg
{

int FastEcuCanTransport::write(std::uint32_t canId, bytes::ByteView payload)
{
    bytes::Bytes frame;
    frame.reserve(payload.size() + 4);
    frame.push_back(bytes::Byte(canId >> 24));
    frame.push_back(bytes::Byte(canId >> 16));
    frame.push_back(bytes::Byte(canId >> 8));
    frame.push_back(bytes::Byte(canId));
    frame.insert(frame.end(), payload.begin(), payload.end());
    serial_->write_serial_data_echo_check(bytes::toQByteArray(frame));
    return static_cast<int>(payload.size());
}

bytes::Bytes FastEcuCanTransport::read(int timeoutMs, std::uint32_t& outId)
{
    const bytes::Bytes raw = bytes::fromQByteArray(serial_->read_serial_data(quint16(timeoutMs)));
    if (raw.size() < 4)
        return {};
    outId = (std::uint32_t(raw[0]) << 24) | (std::uint32_t(raw[1]) << 16) | (std::uint32_t(raw[2]) << 8) | std::uint32_t(raw[3]);
    return bytes::Bytes(raw.begin() + 4, raw.end());
}

bool FastEcuCanTransport::isOpen() const
{
    return serial_->is_serial_port_open();
}

} // namespace cdbg
