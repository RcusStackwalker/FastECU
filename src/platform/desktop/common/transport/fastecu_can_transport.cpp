#include "src/platform/desktop/common/transport/fastecu_can_transport.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

#include <exception>

namespace cdbg
{

fastecu::Result<std::size_t> FastEcuCanTransport::Write(std::uint32_t can_id, bytes::ByteView payload)
{
    try
    {
        if (!serial_ || !serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "CAN adapter disconnected before write");
        }
        bytes::Bytes frame;
        frame.reserve(payload.size() + 4);
        bytes::AppendU32Be(frame, can_id);
        frame.insert(frame.end(), payload.begin(), payload.end());
        serial_->WriteSerialDataEchoCheck(bytes::ToQByteArray(frame));
        if (!serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "CAN adapter disconnected during write");
        }
        return payload.size();
    }
    catch (const std::exception& error)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "CAN driver write exception");
    }
}

fastecu::Result<std::optional<CanFrame>> FastEcuCanTransport::Read(std::chrono::milliseconds timeout,
                                                                   const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "CAN read cancelled before driver call");
    }

    try
    {
        if (!serial_ || !serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "CAN adapter disconnected before read");
        }
        const bytes::Bytes raw =
            bytes::FromQByteArray(serial_->ReadSerialData(fastecu::SaturatingMs<quint16>(timeout)));
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "CAN read cancelled");
        }
        if (!serial_->IsSerialPortOpen())
        {
            return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "CAN adapter disconnected during read");
        }
        if (raw.empty())
        {
            return std::optional<CanFrame>{};
        }
        if (raw.size() < 4)
        {
            return fastecu::Fail(fastecu::ErrorKind::kInternal, "CAN driver returned a truncated frame");
        }
        return std::optional<CanFrame>{CanFrame{bytes::ReadU32Be(raw, 0), bytes::Bytes(raw.begin() + 4, raw.end())}};
    }
    catch (const std::exception& error)
    {
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "CAN read cancelled");
        }
        return fastecu::Fail(fastecu::ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        if (cancellation.Cancelled())
        {
            return fastecu::Fail(fastecu::ErrorKind::kCancelled, "CAN read cancelled");
        }
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "CAN driver read exception");
    }
}

bool FastEcuCanTransport::IsOpen() const
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

} // namespace cdbg
