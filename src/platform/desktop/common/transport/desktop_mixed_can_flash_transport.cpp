#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"
#include "src/backend/ports/duration_cast.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

namespace fastecu::flash
{

DesktopMixedCanFlashTransport::DesktopMixedCanFlashTransport(std::unique_ptr<SerialPortActions> serial)
    : owned_serial_(std::move(serial)), serial_(owned_serial_.get())
{
}

DesktopMixedCanFlashTransport::DesktopMixedCanFlashTransport(SerialPortActions *serial) : serial_(serial)
{
}

DesktopMixedCanFlashTransport::~DesktopMixedCanFlashTransport() = default;

Status DesktopMixedCanFlashTransport::configure(const MixedCanConfig& config)
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (mode_ != Mode::Unconfigured && mode_ != Mode::Closed)
    {
        return fail(ErrorKind::InvalidConfig, "configure() called while mixed CAN transport is already configured");
    }
    stored_config_ = config;
    if (const Status configured = configure_iso(config); !configured.has_value())
    {
        return configured;
    }
    mode_ = Mode::Iso15765;
    return {};
}

Status DesktopMixedCanFlashTransport::open()
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "open() called after close()");
    }
    try
    {
        if (serial_->open_serial_port().isEmpty())
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

Status DesktopMixedCanFlashTransport::close()
{
    owned_serial_.reset();
    serial_ = nullptr;
    mode_ = Mode::Closed;
    return {};
}

Status DesktopMixedCanFlashTransport::enter_raw_bootloader_mode()
{
    if (const Status ready = io_ready(Mode::Iso15765, "raw bootloader transition"); !ready.has_value())
    {
        return ready;
    }
    if (const Status reset = reset_connection(); !reset.has_value())
    {
        return transition_failure(reset);
    }
    if (const Status configured = configure_raw(stored_config_->bootloader); !configured.has_value())
    {
        return transition_failure(configured);
    }
    if (const Status opened = open(); !opened.has_value())
    {
        return transition_failure(opened);
    }
    mode_ = Mode::Raw;
    return {};
}

Status DesktopMixedCanFlashTransport::clear_receive_buffer()
{
    if (const Status ready = io_ready(Mode::Raw, "receive-buffer clear"); !ready.has_value())
    {
        return ready;
    }
    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "mixed CAN adapter disconnected before receive-buffer clear");
        }
        if (serial_->clear_rx_buffer() != STATUS_SUCCESS)
        {
            return fail(ErrorKind::Internal, "clear_rx_buffer failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "clear_rx_buffer exception");
    }
}

Status DesktopMixedCanFlashTransport::enter_iso15765_kernel_mode()
{
    if (const Status ready = io_ready(Mode::Raw, "ISO-15765 kernel transition"); !ready.has_value())
    {
        return ready;
    }
    if (const Status reset = reset_connection(); !reset.has_value())
    {
        return transition_failure(reset);
    }
    if (const Status configured = configure_iso(*stored_config_); !configured.has_value())
    {
        return transition_failure(configured);
    }
    if (const Status opened = open(); !opened.has_value())
    {
        return transition_failure(opened);
    }
    mode_ = Mode::Iso15765;
    return {};
}

Status DesktopMixedCanFlashTransport::write_iso15765(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (const Status ready = io_ready(Mode::Iso15765, "ISO-15765 write"); !ready.has_value())
    {
        return ready;
    }
    return write_serial(data, cancellation);
}

Result<std::optional<bytes::Bytes>> DesktopMixedCanFlashTransport::read_iso15765(std::chrono::milliseconds timeout,
                                                                                 const ICancellationToken& cancellation)
{
    if (const Status ready = io_ready(Mode::Iso15765, "ISO-15765 read"); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }
    return read_serial(timeout, cancellation);
}

Status DesktopMixedCanFlashTransport::write_raw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation)
{
    if (const Status ready = io_ready(Mode::Raw, "raw CAN write"); !ready.has_value())
    {
        return ready;
    }
    if (frame.payload.size() > 8)
    {
        return fail(ErrorKind::InvalidConfig, "raw CAN payload exceeds 8 bytes");
    }
    // Contract: the frame is sent exactly as composed, so DLC equals
    // frame.payload.size() -- unlike the legacy DensoCAN path, which always
    // wrote 8 zero-filled payload bytes. A caller whose target expects a
    // fixed 8-byte DLC must pad frame.payload itself before calling this.
    return write_serial(bytes::composeBe(frame.id, frame.payload), cancellation);
}

Result<std::optional<cdbg::CanFrame>> DesktopMixedCanFlashTransport::read_raw(std::chrono::milliseconds timeout,
                                                                              const ICancellationToken& cancellation)
{
    if (const Status ready = io_ready(Mode::Raw, "raw CAN read"); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }
    const auto raw = read_serial(timeout, cancellation);
    if (!raw.has_value())
    {
        return std::unexpected(raw.error());
    }
    if (!raw->has_value())
    {
        return std::optional<cdbg::CanFrame>{};
    }
    if (raw->value().size() < 4)
    {
        return fail(ErrorKind::BadResponse, "raw CAN response lacks a four-byte arbitration ID");
    }
    const std::uint32_t id = bytes::readU32Be(raw->value());
    if (id != stored_config_->bootloader.receive_id)
    {
        return fail(ErrorKind::BadResponse, std::format("expected raw CAN reply id 0x{:x}, got 0x{:x}",
                                                        stored_config_->bootloader.receive_id, id));
    }
    bytes::Bytes payload(raw->value().begin() + 4, raw->value().end());
    if (payload.size() > 8)
    {
        return fail(ErrorKind::BadResponse, "raw CAN response payload exceeds 8 bytes");
    }
    return std::optional<cdbg::CanFrame>{cdbg::CanFrame{.id = id, .payload = std::move(payload)}};
}

void DesktopMixedCanFlashTransport::request_unblock() noexcept
{
    unblock_requested_.store(true, std::memory_order_release);
}

Status DesktopMixedCanFlashTransport::configure_iso(const MixedCanConfig& config)
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "configure() called after close()");
    }
    try
    {
        if (!serial_->set_is_iso14230_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->set_is_can_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso15765_connection(true))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_29_bit_id(config.kernel.extended_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->set_can_speed(QString::number(config.kernel.bitrate)))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_speed failed");
        }
        // Deliberate, not a copy-paste slip against DesktopCanFlashTransport::
        // configure(): while ISO-15765 is selected, set_j2534_can_filters()
        // (serial_port_actions_direct.cpp:1532-1600) reads only the
        // iso15765_source/destination_address pair set below, so the raw
        // can_source/destination_address values are inert here. They are set
        // anyway to mirror the legacy DensoCAN path, which configures ISO
        // 0x7e0/0x7e8 and then also stamps the raw CAN pair with the
        // bootloader's 0x000FFFFE/0x21
        // (flash_ecu_subaru_denso_sh705x_densocan_operation.cpp:68-70).
        if (!serial_->set_can_source_address(config.bootloader.transmit_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_source_address failed");
        }
        if (!serial_->set_can_destination_address(config.bootloader.receive_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_destination_address failed");
        }
        if (!serial_->set_iso15765_source_address(config.kernel.request_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_iso15765_source_address failed");
        }
        if (!serial_->set_iso15765_destination_address(config.kernel.response_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_iso15765_destination_address failed");
        }
        if (!serial_->set_add_iso14230_header(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_add_iso14230_header failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "mixed CAN ISO-15765 configure exception");
    }
}

Status DesktopMixedCanFlashTransport::configure_raw(const RawCanConfig& config)
{
    try
    {
        if (!serial_->set_is_iso14230_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->set_is_can_connection(true))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->set_is_iso15765_connection(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->set_is_29_bit_id(config.extended_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->set_can_speed(QString::number(config.bitrate)))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_speed failed");
        }
        if (!serial_->set_can_source_address(config.transmit_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_source_address failed");
        }
        if (!serial_->set_can_destination_address(config.receive_id))
        {
            return fail(ErrorKind::InvalidConfig, "set_can_destination_address failed");
        }
        if (!serial_->set_add_iso14230_header(false))
        {
            return fail(ErrorKind::InvalidConfig, "set_add_iso14230_header failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "mixed CAN raw configure exception");
    }
}

Status DesktopMixedCanFlashTransport::reset_connection()
{
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, "reset_connection() called after close()");
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
            return fail(ErrorKind::Internal, "reset_connection failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "reset_connection exception");
    }
}

Status DesktopMixedCanFlashTransport::transition_failure(Status status)
{
    transition_error_ = status.error();
    return status;
}

Status DesktopMixedCanFlashTransport::io_ready(Mode required_mode, std::string_view operation) const
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (!serial_)
    {
        return fail(ErrorKind::Disconnected, std::format("{} called after close()", operation));
    }
    if (mode_ != required_mode)
    {
        return fail(ErrorKind::InvalidConfig, std::format("mixed CAN {} in wrong mode", operation));
    }
    return {};
}

Status DesktopMixedCanFlashTransport::write_serial(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (cancellation.cancelled() || unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "mixed CAN write skipped due to cancellation/unblock");
    }
    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "mixed CAN adapter disconnected before write");
        }
        serial_->write_serial_data_echo_check(bytes::toQByteArray(data));
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "mixed CAN adapter disconnected during write");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        return fail(ErrorKind::Internal, "mixed CAN driver write exception");
    }
}

Result<std::optional<bytes::Bytes>> DesktopMixedCanFlashTransport::read_serial(std::chrono::milliseconds timeout,
                                                                               const ICancellationToken& cancellation)
{
    if (cancellation.cancelled() || unblock_requested_.load(std::memory_order_acquire))
    {
        return fail(ErrorKind::Cancelled, "mixed CAN read skipped due to cancellation/unblock");
    }
    try
    {
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "mixed CAN adapter disconnected before read");
        }
        const QByteArray raw = serial_->read_serial_data(fastecu::saturating_ms<quint16>(timeout));
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "mixed CAN read cancelled");
        }
        if (!serial_->is_serial_port_open())
        {
            return fail(ErrorKind::Disconnected, "mixed CAN adapter disconnected during read");
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
            return fail(ErrorKind::Cancelled, "mixed CAN read cancelled");
        }
        return fail(ErrorKind::Internal, error.what());
    }
    catch (...)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "mixed CAN read cancelled");
        }
        return fail(ErrorKind::Internal, "mixed CAN driver read exception");
    }
}

} // namespace fastecu::flash
