#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

#include <exception>

namespace cdbg
{

fastecu::Result<std::size_t> FastEcuCanTransport::write(
    std::uint32_t canId, bytes::ByteView payload)
{
    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "CAN adapter disconnected before write");
        }
        bytes::Bytes frame;
        frame.reserve(payload.size() + 4);
        bytes::appendU32Be(frame, canId);
        frame.insert(frame.end(), payload.begin(), payload.end());
        serial_->write_serial_data_echo_check(bytes::toQByteArray(frame));
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "CAN adapter disconnected during write");
        }
        return payload.size();
    }
    catch (const std::exception& error)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "CAN driver write exception");
    }
}

fastecu::Result<std::optional<CanFrame>> FastEcuCanTransport::read(
    int timeoutMs, const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "CAN read cancelled before driver call");
    }

    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "CAN adapter disconnected before read");
        }
        const bytes::Bytes raw = bytes::fromQByteArray(serial_->read_serial_data(quint16(timeoutMs)));
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "CAN read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "CAN adapter disconnected during read");
        }
        if (raw.empty())
        {
            return std::optional<CanFrame>{};
        }
        if (raw.size() < 4)
        {
            return fastecu::fail(fastecu::ErrorKind::Internal, "CAN driver returned a truncated frame");
        }
        return std::optional<CanFrame>{CanFrame{
            bytes::readU32Be(raw, 0), bytes::Bytes(raw.begin() + 4, raw.end())}};
    }
    catch (const std::exception& error)
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "CAN read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "CAN read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, "CAN driver read exception");
    }
}

bool FastEcuCanTransport::isOpen() const
{
    try
    {
        return serial_ && serial_->is_serial_port_open();
    }
    catch (...)
    {
        return false;
    }
}

} // namespace cdbg
