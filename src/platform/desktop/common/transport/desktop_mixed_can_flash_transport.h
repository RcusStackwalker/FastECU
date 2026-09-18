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

    Status reset_connection() override;
    Status configure(const MixedCanConfig& config) override;
    Status open() override;
    Status close() override;
    Status enter_raw_bootloader_mode() override;
    Status clear_receive_buffer() override;
    Status enter_iso15765_kernel_mode() override;
    Status write_iso15765(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> read_iso15765(std::chrono::milliseconds timeout,
                                                      const ICancellationToken& cancellation) override;
    Status write_raw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override;
    Result<std::optional<cdbg::CanFrame>> read_raw(std::chrono::milliseconds timeout,
                                                   const ICancellationToken& cancellation) override;
    void request_unblock() noexcept override;

  private:
    enum class Mode
    {
        Unconfigured,
        Iso15765,
        Raw,
        Closed,
    };

    Status configure_iso(const MixedCanConfig& config);
    Status configure_raw(const RawCanConfig& config);
    Status transition_failure(Status status);
    Status io_ready(Mode required_mode, std::string_view operation) const;
    Status write_serial(bytes::ByteView data, const ICancellationToken& cancellation);
    Result<std::optional<bytes::Bytes>> read_serial(std::chrono::milliseconds timeout,
                                                    const ICancellationToken& cancellation);

    std::unique_ptr<SerialPortActions> owned_serial_;
    SerialPortActions *serial_ = nullptr;
    std::optional<MixedCanConfig> stored_config_;
    std::optional<Error> transition_error_;
    Mode mode_ = Mode::Unconfigured;
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
