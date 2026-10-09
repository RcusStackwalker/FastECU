#include "src/platform/desktop/common/transport/serial_read.h"
#include "src/platform/desktop/common/transport/desktop_kline_flash_transport.h"

#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"
#include <QSerialPort>

namespace fastecu::flash
{

DesktopKlineFlashTransport::DesktopKlineFlashTransport(std::unique_ptr<SerialPortActions> serial)
    : owned_serial_(std::move(serial)), serial_(owned_serial_.get())
{
}

DesktopKlineFlashTransport::DesktopKlineFlashTransport(SerialPortActions *serial) : serial_(serial)
{
}

DesktopKlineFlashTransport::~DesktopKlineFlashTransport() = default;

Status DesktopKlineFlashTransport::Configure(const KlineConfig& config)
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "configure() called after close()");
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
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->set_is_can_connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso15765_connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_29_bit_id(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->set_serial_port_baudrate(QString::number(config.baud)))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_serial_port_baudrate failed");
        }
        const auto parity = config.parity == KlineParity::kEven  ? QSerialPort::EvenParity
                            : config.parity == KlineParity::kOdd ? QSerialPort::OddParity
                                                                 : QSerialPort::NoParity;
        if (!serial_->set_serial_port_parity(static_cast<std::uint8_t>(parity)))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_serial_port_parity failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "K-Line configure exception");
    }
}

Status DesktopKlineFlashTransport::Open()
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "open() called after close()");
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
        const QString open_result = serial_->open_serial_port();
        if (open_result.isEmpty())
        {
            return Fail(ErrorKind::kDisconnected, "open_serial_port failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kDisconnected, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kDisconnected, "open_serial_port exception");
    }
}

Status DesktopKlineFlashTransport::Close()
{
    // Idempotent: resetting an already-null unique_ptr, and clearing an
    // already-null raw pointer, are both no-ops. Only owned_serial_ is ever
    // destroyed here -- a non-owning serial_ (raw-pointer constructor) is
    // just forgotten, never deleted; the caller that gave it to us keeps
    // owning its lifetime (see the non-owning constructor's header comment).
    owned_serial_.reset();
    serial_ = nullptr;
    return {};
}

Status DesktopKlineFlashTransport::ResetConnection()
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "reset_connection() called after close()");
    }
    try
    {
        // No real sentinel: SerialPortActions::reset_connection()
        // (serial_port_actions.cpp:541-544) is `runOnBackend(...); return
        // true;` -- it cannot report failure through its return value, so
        // this branch is unreachable today and only the surrounding catch
        // blocks below can produce an Internal error here.
        if (!serial_->reset_connection())
        {
            return Fail(ErrorKind::kInternal, "reset_connection failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "reset_connection exception");
    }
}

Status DesktopKlineFlashTransport::DisableLecLines()
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "disable_lec_lines() called after close()");
    }
    try
    {
        if (serial_->set_lec_lines(serial_->get_requestToSendDisabled(), serial_->get_dataTerminalDisabled()) != 0)
        {
            return Fail(ErrorKind::kInternal, "set_lec_lines disabled failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "disable_lec_lines exception");
    }
}

Status DesktopKlineFlashTransport::PulseLec2Line(std::chrono::milliseconds timeout)
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "pulse_lec_2_line() called after close()");
    }
    try
    {
        if (serial_->pulse_lec_2_line(fastecu::SaturatingMs<int>(timeout)) != 0)
        {
            return Fail(ErrorKind::kInternal, "pulse_lec_2_line failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "pulse_lec_2_line exception");
    }
}

Status DesktopKlineFlashTransport::EnableProgrammingVoltageLine()
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "enable_programming_voltage_line() called after close()");
    }
    try
    {
        // Legacy src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_mc68hc16y5_02_operation.cpp:513.
        if (serial_->set_lec_lines(serial_->get_requestToSendEnabled(), serial_->get_dataTerminalDisabled()) != 0)
        {
            return Fail(ErrorKind::kInternal, "set_lec_lines programming state failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "enable_programming_voltage_line exception");
    }
}

Status DesktopKlineFlashTransport::EnableBootModeLines()
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "enable_boot_mode_lines() called after close()");
    }
    try
    {
        // Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp:66.
        if (serial_->set_lec_lines(serial_->get_requestToSendEnabled(), serial_->get_dataTerminalEnabled()) != 0)
        {
            return Fail(ErrorKind::kInternal, "set_lec_lines boot mode state failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "enable_boot_mode_lines exception");
    }
}

bool DesktopKlineFlashTransport::RequiresPostKernelUploadDelay() const
{
#if defined(Q_OS_UNIX)
    return serial_ != nullptr && serial_->get_use_openport2_adapter();
#else
    return false;
#endif
}

Status DesktopKlineFlashTransport::SetAddIso14230Header(bool add_header)
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "set_add_iso14230_header() called after close()");
    }

    try
    {
        // Same shape as configure()'s setters above: SerialPortActionsDirect::
        // set_add_iso14230_header() (serial_port_actions_direct.h:217-221)
        // just assigns a member and unconditionally `return true` -- it
        // cannot plausibly report "adapter gone", so any failure here is
        // InvalidConfig, never Disconnected.
        if (!serial_->set_add_iso14230_header(add_header))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_add_iso14230_header failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "set_add_iso14230_header exception");
    }
}

void DesktopKlineFlashTransport::RequestUnblock() noexcept
{
    // Best-effort: SerialPortActions has no interrupt primitive, so an
    // in-flight read_serial_data(timeout) call still returns on its own
    // existing bounded timeout (<=3000ms per the design spec's
    // characterized values). Setting this flag only guarantees no *further*
    // read/write is issued once request_unblock() has fired.
    //
    // Default (seq_cst) ordering: the flag is a pure signal that guards no
    // other data, so there is nothing for an acquire/release pair to
    // publish, and the barrier is free next to the serial I/O it gates.
    unblock_requested_.store(true);
}

Status DesktopKlineFlashTransport::SetBaud(int baud)
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "setBaud() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected before baud change");
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
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected during baud change");
        }
        // Port is still open but the driver rejected the change (e.g. the
        // generic-adapter branch's serial->setBaudRate() call failed) --
        // this is a runtime driver failure, not a config-shape problem, so
        // it maps to Internal rather than InvalidConfig (mirrors
        // FastEcuKlineTransport::setBaud() in this same package, which
        // wraps the identical change_port_speed() call).
        return Fail(ErrorKind::kInternal, "K-Line driver rejected baud change");
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "K-Line driver baud-change exception");
    }
}

Result<std::size_t> DesktopKlineFlashTransport::Write(bytes::ByteView data)
{
    if (unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "K-Line write skipped after request_unblock");
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "write() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected before write");
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
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "K-Line driver write exception");
    }
}

Result<DesktopKlineFlashTransport::OptionalBytes>
DesktopKlineFlashTransport::Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled() || unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "K-Line read skipped due to cancellation/unblock");
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "read() called after close()");
    }

    return fastecu::desktop::detail::read_serial(serial_, timeout, cancellation, [this](std::uint16_t driver_timeout)
                                                 { return serial_->read_serial_data(driver_timeout); });
}

Result<std::size_t> DesktopKlineFlashTransport::WriteRaw(bytes::ByteView data)
{
    if (unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "K-Line write skipped after request_unblock");
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "write_raw() called after close()");
    }

    try
    {
        if (!serial_->is_serial_port_open())
        {
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected before write");
        }
        // write_serial_data()'s QByteArray return cannot signal
        // success/failure: every path through SerialPortActionsDirect::
        // write_serial_data() (serial_port_actions_direct.cpp:
        // 939-1002) -- including the echo-timeout path -- ends with
        // `return STATUS_SUCCESS;` (0, implicitly converted to a null
        // QByteArray). Every legacy K-Line flash caller (e.g.
        // flash_ecu_subaru_mitsu_m32r_kline_operation.cpp) already calls it
        // as a bare statement and discards the result for exactly this
        // reason. is_serial_port_open() is the only reliable
        // post-condition, matching FastEcuCanTransport::write_raw() in this
        // same package (which wraps the same call for the CDBG protocol).
        serial_->write_serial_data(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return Fail(ErrorKind::kDisconnected, "K-Line adapter disconnected during write");
        }
        return data.size();
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "K-Line driver write exception");
    }
}

Result<DesktopKlineFlashTransport::OptionalBytes>
DesktopKlineFlashTransport::ReadRaw(std::chrono::milliseconds timeout, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled() || unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "K-Line read skipped due to cancellation/unblock");
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "read_raw() called after close()");
    }

    return fastecu::desktop::detail::read_serial(serial_, timeout, cancellation, [this](std::uint16_t driver_timeout)
                                                 { return serial_->read_serial_obd_data(driver_timeout); });
}

bool DesktopKlineFlashTransport::IsOpen() const
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

bool adapter_supplies_programming_voltage(SerialPortActions *serial)
{
    return serial != nullptr && serial->get_use_openport2_adapter();
}

} // namespace fastecu::flash
