#pragma once

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Borrows the transport. Only Read/Write refuse cancellation at call entry;
// accepted outcomes and lifecycle operations retain their original meaning.
class GuardedCanFlashTransport final : public ICanFlashTransport
{
  public:
    explicit GuardedCanFlashTransport(ICanFlashTransport& inner) noexcept;
    Status ResetConnection() override;
    Status Configure(const Iso15765Config& config) override;
    Status Open() override;
    Status Close() override;
    Status RestartIso15765(const Iso15765Config& config, const ICancellationToken& cancellation) override;
    void RequestUnblock() noexcept override;
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override;

  private:
    ICanFlashTransport& inner_;
};

} // namespace fastecu::flash
