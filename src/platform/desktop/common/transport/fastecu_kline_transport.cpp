#include "src/platform/desktop/common/transport/fastecu_kline_transport.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

#include <exception>

namespace mutdma
{
fastecu::Status FastEcuKlineTransport::setBaud(int baud)
{
    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected before baud change");
        }
        if (serial_->change_port_speed(QString::number(baud)) == 0)
        {
            return {};
        }
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected during baud change");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, "K-Line driver rejected baud change");
    }
    catch (const std::exception& error)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "K-Line driver baud-change exception");
    }
}

fastecu::Result<std::size_t> FastEcuKlineTransport::write(bytes::ByteView data)
{
    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected before write");
        }
        serial_->write_serial_data(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "K-Line driver write exception");
    }
}

fastecu::Result<IKlineTransport::OptionalBytes> FastEcuKlineTransport::read(
    int timeoutMs, const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "K-Line read cancelled before driver call");
    }

    try
    {
        if (!serial_ || !serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected before read");
        }
        const QByteArray raw = serial_->read_serial_data(quint16(timeoutMs));
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "K-Line read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fastecu::fail(fastecu::ErrorKind::Disconnected, "K-Line adapter disconnected during read");
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
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "K-Line read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fastecu::fail(fastecu::ErrorKind::Cancelled, "K-Line read cancelled");
        }
        return fastecu::fail(fastecu::ErrorKind::Internal, "K-Line driver read exception");
    }
}
bool FastEcuKlineTransport::isOpen() const
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
} // namespace mutdma
