#include "src/platform/desktop/common/transport/desktop_can_flash_transport.h"

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::flash
{

DesktopCanFlashTransport::DesktopCanFlashTransport(std::unique_ptr<SerialPortActions> serial)
    : owned_serial_(std::move(serial)), serial_(owned_serial_.get())
{
}

DesktopCanFlashTransport::DesktopCanFlashTransport(SerialPortActions *serial)
    : owned_serial_(), serial_(serial)
{
}

DesktopCanFlashTransport::~DesktopCanFlashTransport() = default;

Status DesktopCanFlashTransport::configure(const Iso15765Config& config)
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "configure() called after close()");
    }

    try
    {
        // Every setter below (SerialPortActionsDirect, the real backend --
        // see serial_port_actions_direct.h) just assigns a member and
        // unconditionally `return true`; none of them probe the port or
        // touch hardware, so none can plausibly report "adapter gone" --
        // every failure here is InvalidConfig, never Disconnected. open(),
        // below, is the one call in this adapter that actually touches
        // hardware and maps failure to Disconnected. Order matches the
        // design spec exactly: connection mode, 11/29-bit selection,
        // bitrate, request/response CAN IDs, then ISO-15765 source/
        // destination IDs.
        if (!serial_->set_is_iso15765_connection(true))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_can_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso14230_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->set_is_29_bit_id(config.extended_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->set_can_speed(QString::number(config.bitrate)))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_speed failed");
        }
        if (!serial_->set_can_source_address(config.request_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_source_address failed");
        }
        if (!serial_->set_can_destination_address(config.response_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_destination_address failed");
        }
        if (!serial_->set_iso15765_source_address(config.request_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_iso15765_source_address failed");
        }
        if (!serial_->set_iso15765_destination_address(config.response_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_iso15765_destination_address failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "CAN configure exception");
    }
}

Status DesktopCanFlashTransport::open()
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "open() called after close()");
    }

    try
    {
        // Real sentinel, confirmed by reading the body (step 5c Task 12):
        // SerialPortActionsDirect::open_serial_port()
        // (serial_port_actions_direct.cpp:519-644) returns `openedSerialPort`
        // (non-empty) on every success path and `{}` (an empty/null QString)
        // on every failure path.
        const QString openResult = serial_->open_serial_port();
        if (openResult.isEmpty())
        {
            return fail(ErrorKind::Disconnected, "open_serial_port failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Disconnected, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Disconnected, "open_serial_port exception");
    }
}

Status DesktopCanFlashTransport::close()
{
    // Idempotent, and non-destructive of a non-owning serial_ -- see
    // DesktopKlineFlashTransport::close() for the full rationale.
    owned_serial_.reset();
    serial_ = nullptr;
    return {};
}

void DesktopCanFlashTransport::request_unblock() noexcept
{
    // Best-effort: SerialPortActions has no interrupt primitive, so an
    // in-flight read_serial_data(timeout) call still returns on its own
    // existing bounded timeout (<=3000ms per the design spec's
    // characterized values). Setting this flag only guarantees no *further*
    // read/write is issued once request_unblock() has fired.
    unblock_requested_.store(true, std::memory_order_release);
}

Status DesktopCanFlashTransport::write(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (cancellation.cancelled() || unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "CAN write skipped due to cancellation/unblock");
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "write() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "CAN adapter disconnected before write");
        }
        // write_serial_data_echo_check()'s QByteArray return cannot signal
        // success/failure: every path through SerialPortActionsDirect::
        // write_serial_data_echo_check() (serial_port_actions_direct.cpp:
        // 939-1002) -- including the echo-timeout path -- ends with
        // `return STATUS_SUCCESS;` (0, implicitly converted to a null
        // QByteArray), and the accumulated echo bytes are discarded
        // afterward. is_serial_port_open() is the only reliable
        // post-condition, matching FastEcuCanTransport::write() in this
        // same package (which wraps the identical call).
        serial_->write_serial_data_echo_check(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "CAN adapter disconnected during write");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "CAN driver write exception");
    }
}

Result<std::optional<bytes::Bytes>> DesktopCanFlashTransport::read(
    int timeout_ms, const ICancellationToken& cancellation)
{
    if (cancellation.cancelled() || unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "CAN read skipped due to cancellation/unblock");
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "read() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "CAN adapter disconnected before read");
        }
        const QByteArray raw = serial_->read_serial_data(static_cast<quint16>(timeout_ms));
        // Deliberately NOT re-checking unblock_requested_ here: this call
        // was already in flight when request_unblock() may have fired, and
        // the documented contract is that such a call still returns via its
        // own existing timeout with whatever the backend actually produced
        // -- request_unblock() only suppresses the *next* read/write, not
        // retroactively discard one already past the point of no return.
        // (cancellation.cancelled() IS still re-checked here, matching
        // FastEcuCanTransport::read() in this same package -- teardown
        // cancellation and the unblock flag are different contracts.)
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "CAN read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "CAN adapter disconnected during read");
        }
        if (raw.isEmpty())
        {
            return std::optional<bytes::Bytes>{};
        }
        return std::optional<bytes::Bytes>{bytes::fromQByteArray(raw)};
    }
    catch (const std::exception& error)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "CAN read cancelled");
        }
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "CAN read cancelled");
        }
        return fail(ErrorKind::Internal, "CAN driver read exception");
    }
}

} // namespace fastecu::flash
