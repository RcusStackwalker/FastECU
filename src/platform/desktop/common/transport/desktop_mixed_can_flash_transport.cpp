#include "src/platform/desktop/common/transport/serial_read.h"
#include "src/platform/desktop/common/transport/desktop_mixed_can_flash_transport.h"

#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/platform/desktop/common/serial/facade/serial_port_actions.h"

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

Status DesktopMixedCanFlashTransport::Configure(const MixedCanConfig& config)
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (mode_ != Mode::kUnconfigured && mode_ != Mode::kClosed)
    {
        return Fail(ErrorKind::kInvalidConfig, "configure() called while mixed CAN transport is already configured");
    }
    stored_config_ = config;
    if (const Status configured = ConfigureIso(config); !configured.has_value())
    {
        return configured;
    }
    mode_ = Mode::kIso15765;
    return {};
}

Status DesktopMixedCanFlashTransport::Open()
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "open() called after close()");
    }
    try
    {
        if (serial_->OpenSerialPort().isEmpty())
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

Status DesktopMixedCanFlashTransport::Close()
{
    owned_serial_.reset();
    serial_ = nullptr;
    mode_ = Mode::kClosed;
    return {};
}

Status DesktopMixedCanFlashTransport::EnterRawBootloaderMode()
{
    if (const Status ready = IoReady(Mode::kIso15765, "raw bootloader transition"); !ready.has_value())
    {
        return ready;
    }
    if (!stored_config_.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "mixed CAN transport has no stored configuration");
    }
    const MixedCanConfig config = *stored_config_;
    if (const Status reset = ResetConnection(); !reset.has_value())
    {
        return TransitionFailure(reset);
    }
    if (const Status configured = ConfigureRaw(config.bootloader); !configured.has_value())
    {
        return TransitionFailure(configured);
    }
    if (const Status opened = Open(); !opened.has_value())
    {
        return TransitionFailure(opened);
    }
    mode_ = Mode::kRaw;
    return {};
}

Status DesktopMixedCanFlashTransport::ClearReceiveBuffer()
{
    if (const Status ready = IoReady(Mode::kRaw, "receive-buffer clear"); !ready.has_value())
    {
        return ready;
    }
    try
    {
        if (!serial_->IsSerialPortOpen())
        {
            return Fail(ErrorKind::kDisconnected, "mixed CAN adapter disconnected before receive-buffer clear");
        }
        if (serial_->ClearRxBuffer() != kSerialSuccess)
        {
            return Fail(ErrorKind::kInternal, "clear_rx_buffer failed");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "clear_rx_buffer exception");
    }
}

Status DesktopMixedCanFlashTransport::EnterIso15765KernelMode()
{
    if (const Status ready = IoReady(Mode::kRaw, "ISO-15765 kernel transition"); !ready.has_value())
    {
        return ready;
    }
    if (!stored_config_.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "mixed CAN transport has no stored configuration");
    }
    const MixedCanConfig config = *stored_config_;
    if (const Status reset = ResetConnection(); !reset.has_value())
    {
        return TransitionFailure(reset);
    }
    if (const Status configured = ConfigureIso(config); !configured.has_value())
    {
        return TransitionFailure(configured);
    }
    if (const Status opened = Open(); !opened.has_value())
    {
        return TransitionFailure(opened);
    }
    mode_ = Mode::kIso15765;
    return {};
}

Status DesktopMixedCanFlashTransport::WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (const Status ready = IoReady(Mode::kIso15765, "ISO-15765 write"); !ready.has_value())
    {
        return ready;
    }
    return WriteSerial(data, cancellation);
}

Result<std::optional<bytes::Bytes>> DesktopMixedCanFlashTransport::ReadIso15765(std::chrono::milliseconds timeout,
                                                                                const ICancellationToken& cancellation)
{
    if (const Status ready = IoReady(Mode::kIso15765, "ISO-15765 read"); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }
    return ReadSerial(timeout, cancellation);
}

Status DesktopMixedCanFlashTransport::WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation)
{
    if (const Status ready = IoReady(Mode::kRaw, "raw CAN write"); !ready.has_value())
    {
        return ready;
    }
    if (frame.payload.size() > 8)
    {
        return Fail(ErrorKind::kInvalidConfig, "raw CAN payload exceeds 8 bytes");
    }
    // Contract: the frame is sent exactly as composed, so DLC equals
    // frame.payload.size() -- unlike the legacy DensoCAN path, which always
    // wrote 8 zero-filled payload bytes. A caller whose target expects a
    // fixed 8-byte DLC must pad frame.payload itself before calling this.
    return WriteSerial(bytes::ComposeBe(frame.id, frame.payload), cancellation);
}

Result<std::optional<cdbg::CanFrame>> DesktopMixedCanFlashTransport::ReadRaw(std::chrono::milliseconds timeout,
                                                                             const ICancellationToken& cancellation)
{
    if (const Status ready = IoReady(Mode::kRaw, "raw CAN read"); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }
    const auto raw = ReadSerial(timeout, cancellation);
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
        return Fail(ErrorKind::kBadResponse, "raw CAN response lacks a four-byte arbitration ID");
    }
    const std::uint32_t id = bytes::ReadU32Be(raw->value());
    if (!stored_config_.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, "mixed CAN transport has no stored configuration");
    }
    if (id != stored_config_->bootloader.receive_id)
    {
        return Fail(ErrorKind::kBadResponse, std::format("expected raw CAN reply id 0x{:x}, got 0x{:x}",
                                                         stored_config_->bootloader.receive_id, id));
    }
    bytes::Bytes payload(raw->value().begin() + 4, raw->value().end());
    if (payload.size() > 8)
    {
        return Fail(ErrorKind::kBadResponse, "raw CAN response payload exceeds 8 bytes");
    }
    return std::optional<cdbg::CanFrame>{cdbg::CanFrame{.id = id, .payload = std::move(payload)}};
}

void DesktopMixedCanFlashTransport::RequestUnblock() noexcept
{
    unblock_requested_.store(true);
}

Status DesktopMixedCanFlashTransport::ConfigureIso(const MixedCanConfig& config)
{
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, "configure() called after close()");
    }
    try
    {
        if (!serial_->SetIsIso14230Connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->SetIsCanConnection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->SetIsIso15765Connection(true))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->SetIs29BitId(config.kernel.extended_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->SetCanSpeed(QString::number(config.kernel.bitrate)))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_speed failed");
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
        if (!serial_->SetCanSourceAddress(config.bootloader.transmit_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_source_address failed");
        }
        if (!serial_->SetCanDestinationAddress(config.bootloader.receive_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_destination_address failed");
        }
        if (!serial_->SetIso15765SourceAddress(config.kernel.request_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_iso15765_source_address failed");
        }
        if (!serial_->SetIso15765DestinationAddress(config.kernel.response_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_iso15765_destination_address failed");
        }
        if (!serial_->SetAddIso14230Header(false))
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
        return Fail(ErrorKind::kInternal, "mixed CAN ISO-15765 configure exception");
    }
}

Status DesktopMixedCanFlashTransport::ConfigureRaw(const RawCanConfig& config)
{
    try
    {
        if (!serial_->SetIsIso14230Connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso14230_connection failed");
        }
        if (!serial_->SetIsCanConnection(true))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_can_connection failed");
        }
        if (!serial_->SetIsIso15765Connection(false))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_iso15765_connection failed");
        }
        if (!serial_->SetIs29BitId(config.extended_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_is_29_bit_id failed");
        }
        if (!serial_->SetCanSpeed(QString::number(config.bitrate)))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_speed failed");
        }
        if (!serial_->SetCanSourceAddress(config.transmit_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_source_address failed");
        }
        if (!serial_->SetCanDestinationAddress(config.receive_id))
        {
            return Fail(ErrorKind::kInvalidConfig, "set_can_destination_address failed");
        }
        if (!serial_->SetAddIso14230Header(false))
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
        return Fail(ErrorKind::kInternal, "mixed CAN raw configure exception");
    }
}

Status DesktopMixedCanFlashTransport::ResetConnection()
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
        if (!serial_->ResetConnection())
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

Status DesktopMixedCanFlashTransport::TransitionFailure(Status status)
{
    transition_error_ = status.error();
    return status;
}

Status DesktopMixedCanFlashTransport::IoReady(Mode required_mode, std::string_view operation) const
{
    if (transition_error_)
    {
        return std::unexpected(*transition_error_);
    }
    if (!serial_)
    {
        return Fail(ErrorKind::kDisconnected, std::format("{} called after close()", operation));
    }
    if (mode_ != required_mode)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("mixed CAN {} in wrong mode", operation));
    }
    return {};
}

Status DesktopMixedCanFlashTransport::WriteSerial(bytes::ByteView data, const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled() || unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN write skipped due to cancellation/unblock");
    }
    try
    {
        if (!serial_->IsSerialPortOpen())
        {
            return Fail(ErrorKind::kDisconnected, "mixed CAN adapter disconnected before write");
        }
        serial_->WriteSerialDataEchoCheck(bytes::ToQByteArray(data));
        if (!serial_->IsSerialPortOpen())
        {
            return Fail(ErrorKind::kDisconnected, "mixed CAN adapter disconnected during write");
        }
        return {};
    }
    catch (const std::exception& error)
    {
        return Fail(ErrorKind::kInternal, error.what());
    }
    catch (...)
    {
        return Fail(ErrorKind::kInternal, "mixed CAN driver write exception");
    }
}

Result<std::optional<bytes::Bytes>> DesktopMixedCanFlashTransport::ReadSerial(std::chrono::milliseconds timeout,
                                                                              const ICancellationToken& cancellation)
{
    if (cancellation.Cancelled() || unblock_requested_.load())
    {
        return Fail(ErrorKind::kCancelled, "mixed CAN read skipped due to cancellation/unblock");
    }
    return fastecu::desktop::detail::ReadSerial(serial_, timeout, cancellation, [this](std::uint16_t driver_timeout)
                                                { return serial_->ReadSerialData(driver_timeout); });
}

} // namespace fastecu::flash
