#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

#include "src/backend/flash/flash_executor.h"

class SerialPortActions;

namespace fastecu::flash
{

class DesktopMixedCanFlashTransport final : public IMixedCanFlashTransport
{
  public:
    explicit DesktopMixedCanFlashTransport(std::unique_ptr<SerialPortActions> serial);
    explicit DesktopMixedCanFlashTransport(SerialPortActions *serial);
    ~DesktopMixedCanFlashTransport() override;

    Status ResetConnection() override;
    Status Configure(const MixedCanConfig& config) override;
    Status Open() override;
    Status Close() override;
    Status EnterRawBootloaderMode() override;
    Status ClearReceiveBuffer() override;
    Status EnterIso15765KernelMode() override;
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds timeout,
                                                     const ICancellationToken& cancellation) override;
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override;
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds timeout,
                                                  const ICancellationToken& cancellation) override;
    void RequestUnblock() noexcept override;

  private:
    enum class Mode
    {
        kUnconfigured,
        kIso15765,
        kRaw,
        kClosed,
    };

    Status ConfigureIso(const MixedCanConfig& config);
    Status ConfigureRaw(const RawCanConfig& config);
    Status TransitionFailure(Status status);
    Status IoReady(Mode required_mode, std::string_view operation) const;
    Status WriteSerial(bytes::ByteView data, const ICancellationToken& cancellation);
    Result<std::optional<bytes::Bytes>> ReadSerial(std::chrono::milliseconds timeout,
                                                   const ICancellationToken& cancellation);

    std::unique_ptr<SerialPortActions> owned_serial_;
    SerialPortActions *serial_ = nullptr;
    std::optional<MixedCanConfig> stored_config_;
    std::optional<Error> transition_error_;
    Mode mode_ = Mode::kUnconfigured;
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
