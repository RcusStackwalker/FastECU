#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Borrows the transport. Only raw/ISO I/O refuses cancellation at call entry;
// accepted outcomes, lifecycle, modes and buffer control retain their meaning.
class GuardedMixedCanFlashTransport final : public IMixedCanFlashTransport
{
  public:
    explicit GuardedMixedCanFlashTransport(IMixedCanFlashTransport& inner) noexcept;
    Status ResetConnection() override;
    Status Configure(const MixedCanConfig& config) override;
    Status Open() override;
    Status Close() override;
    Status EnterRawBootloaderMode() override;
    Status ClearReceiveBuffer() override;
    Status EnterIso15765KernelMode() override;
    void RequestUnblock() noexcept override;
    Status WriteIso15765(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> ReadIso15765(std::chrono::milliseconds timeout,
                                                     const ICancellationToken& cancellation) override;
    Status WriteRaw(const cdbg::CanFrame& frame, const ICancellationToken& cancellation) override;
    Result<std::optional<cdbg::CanFrame>> ReadRaw(std::chrono::milliseconds timeout,
                                                  const ICancellationToken& cancellation) override;

  private:
    IMixedCanFlashTransport& inner_;
};

} // namespace fastecu::flash
