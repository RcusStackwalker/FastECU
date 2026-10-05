#include "src/platform/desktop/common/transport/serial_read.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

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

fastecu::Result<ISsmTransport::OptionalBytes> FastEcuSsmTransport::read(std::chrono::milliseconds timeout,
                                                                        const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM read cancelled before driver call");
    }

    return fastecu::desktop::detail::read_serial(
        serial_, timeout, cancellation, fastecu::desktop::detail::kSsmReadErrors,
        [this](std::uint16_t driver_timeout) { return serial_->read_serial_data(driver_timeout); });
}

bool FastEcuSsmTransport::isOpen() const
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
