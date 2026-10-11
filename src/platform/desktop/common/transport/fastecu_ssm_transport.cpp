#include "src/platform/desktop/common/transport/serial_read.h"
#include "src/platform/desktop/common/transport/fastecu_ssm_transport.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

#include <exception>

fastecu::Result<std::size_t> FastEcuSsmTransport::Write(bytes::ByteView data)
{
    try
    {
        if (!serial_ || !serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "SSM adapter disconnected before write");
        }
        serial_->WriteSerialDataEchoCheck(bytes::ToQByteArray(data));
        if (!serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "SSM adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "SSM driver write exception");
    }
}

fastecu::Result<fastecu::ISsmTransport::OptionalBytes>
FastEcuSsmTransport::Read(std::chrono::milliseconds timeout, const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "SSM read cancelled before driver call");
    }

    return fastecu::desktop::detail::ReadSerial(serial_, timeout, cancellation, [this](std::uint16_t driver_timeout)
                                                { return serial_->ReadSerialData(driver_timeout); });
}

bool FastEcuSsmTransport::IsOpen() const
{
    try
    {
        return serial_ && serial_->IsSerialPortOpen();
    }
    catch (...)
    {
        return false;
    }
}
