#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

#include <exception>

fastecu::Result<std::size_t> FastEcuSsmTransport::write(bytes::ByteView data)
{
    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "SSM adapter disconnected before write");
        }
        serial_->write_serial_data_echo_check(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "SSM adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "SSM driver write exception");
    }
}

fastecu::Result<ISsmTransport::OptionalBytes> FastEcuSsmTransport::read(
    int timeoutMs, const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM read cancelled before driver call");
    }

    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "SSM adapter disconnected before read");
        }
        const QByteArray raw = serial_->read_serial_data(static_cast<uint16_t>(timeoutMs));
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "SSM adapter disconnected during read");
        }
        if (raw.isEmpty())
        {
            return OptionalBytes{};
        }
        return OptionalBytes{bytes::fromQByteArray(raw)};
    }
    catch (const std::exception& error)
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, "SSM driver read exception");
    }
}

bool FastEcuSsmTransport::isOpen() const
{
    return serial_ && serial_->is_serial_port_open();
}
