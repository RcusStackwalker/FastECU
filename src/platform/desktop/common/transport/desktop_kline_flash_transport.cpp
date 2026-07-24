#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"

#include "src/algorithms/protocol/qt_bytes.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::flash
{

DesktopKlineFlashTransport::DesktopKlineFlashTransport(std::unique_ptr<SerialPortActions> serial)
    : serial_(std::move(serial))
{
}

DesktopKlineFlashTransport::~DesktopKlineFlashTransport() = default;

Status DesktopKlineFlashTransport::configure(const KlineConfig& config)
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
        // hardware and maps failure to Disconnected.
        if (!serial_->set_is_iso14230_connection(config.iso14230))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->set_is_can_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso15765_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_29_bit_id(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->set_serial_port_baudrate(QString::number(config.baud)))
        {
            return fail(ErrorKind::InvalidConfig, "set_serial_port_baudrate failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "K-Line configure exception");
    }
}

Status DesktopKlineFlashTransport::open()
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
        // on every failure path -- the brief's original guess of "empty
        // QString means failure" happened to be correct here (unlike
        // change_port_speed()'s sentinel below in setBaud()).
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

Status DesktopKlineFlashTransport::close()
{
    serial_.reset(); // idempotent: resetting an already-null unique_ptr is a no-op
    return {};
}

void DesktopKlineFlashTransport::request_unblock() noexcept
{
    // Best-effort: SerialPortActions has no interrupt primitive, so an
    // in-flight read_serial_data(timeout) call still returns on its own
    // existing bounded timeout (<=3000ms per the design spec's
    // characterized values). Setting this flag only guarantees no *further*
    // read/write is issued once request_unblock() has fired.
    unblock_requested_.store(true, std::memory_order_release);
}

Status DesktopKlineFlashTransport::setBaud(int baud)
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "setBaud() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected before baud change");
        }
        // Real sentinel, confirmed by reading the body (step 5c Task 12):
        // SerialPortActionsDirect::change_port_speed()
        // (serial_port_actions_direct.cpp:71-120) returns STATUS_SUCCESS
        // (0x00) on success and STATUS_ERROR (0x01) -- a small *positive*
        // value, never negative -- on every failure path. The brief's
        // original draft guessed `< 0`, which would have silently treated
        // every real failure as success.
        if (serial_->change_port_speed(QString::number(baud)) == 0)
        {
            return {};
        }
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected during baud change");
        }
        // Port is still open but the driver rejected the change (e.g. the
        // generic-adapter branch's serial->setBaudRate() call failed) --
        // this is a runtime driver failure, not a config-shape problem, so
        // it maps to Internal rather than InvalidConfig (mirrors
        // FastEcuKlineTransport::setBaud() in this same package, which
        // wraps the identical change_port_speed() call).
        return fail(ErrorKind::Internal, "K-Line driver rejected baud change");
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "K-Line driver baud-change exception");
    }
}

Result<std::size_t> DesktopKlineFlashTransport::write(bytes::ByteView data)
{
    if (unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "K-Line write skipped after request_unblock");
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "write() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected before write");
        }
        // write_serial_data_echo_check()'s QByteArray return cannot signal
        // success/failure: every path through SerialPortActionsDirect::
        // write_serial_data_echo_check() (serial_port_actions_direct.cpp:
        // 939-1002) -- including the echo-timeout path -- ends with
        // `return STATUS_SUCCESS;` (0, implicitly converted to a null
        // QByteArray). Every legacy K-Line flash caller (e.g.
        // flash_ecu_subaru_mitsu_m32r_kline_operation.cpp) already calls it
        // as a bare statement and discards the result for exactly this
        // reason. is_serial_port_open() is the only reliable
        // post-condition, matching FastEcuCanTransport::write() in this
        // same package (which wraps the same call for the CDBG protocol).
        serial_->write_serial_data_echo_check(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "K-Line driver write exception");
    }
}

Result<DesktopKlineFlashTransport::OptionalBytes> DesktopKlineFlashTransport::read(
    int timeout_ms, const ICancellationToken& cancellation)
{
    if (cancellation.cancelled() || unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "K-Line read skipped due to cancellation/unblock");
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "read() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected before read");
        }
        const QByteArray raw = serial_->read_serial_data(static_cast<quint16>(timeout_ms));
        // Deliberately NOT re-checking unblock_requested_ here: this call
        // was already in flight when request_unblock() may have fired, and
        // the documented contract is that such a call still returns via its
        // own existing timeout with whatever the backend actually produced
        // -- request_unblock() only suppresses the *next* read/write, not
        // retroactively discard one already past the point of no return.
        // (cancellation.cancelled() IS still re-checked here, matching
        // FastEcuKlineTransport::read() in this same package -- teardown
        // cancellation and the unblock flag are different contracts.)
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "K-Line read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "K-Line adapter disconnected during read");
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
            return fail(ErrorKind::Cancelled, "K-Line read cancelled");
        }
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "K-Line read cancelled");
        }
        return fail(ErrorKind::Internal, "K-Line driver read exception");
    }
}

bool DesktopKlineFlashTransport::isOpen() const
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

} // namespace fastecu::flash
