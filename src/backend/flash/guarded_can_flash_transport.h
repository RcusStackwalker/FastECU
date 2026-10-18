#pragma once
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Enforces flash cancellation at the transport boundary (ADR 0025): once the
// constructor token is cancelled, Write and Read refuse with kCancelled
// without touching the inner transport, so no executor can send another
// request or wait for another reply after cancel. Lifecycle calls always
// delegate, so teardown (Close) is never refused.
//
// RestartIso15765 is deliberately not overridden: the inherited default
// drives the guarded lifecycle methods above.
//
// Not thread-safe except RequestUnblock(), which only forwards to the inner
// transport.
class GuardedCanFlashTransport final : public ICanFlashTransport
{
  public:
    GuardedCanFlashTransport(ICanFlashTransport& inner, const ICancellationToken& cancellation);

    Status ResetConnection() override;
    Status Configure(const Iso15765Config& config) override;
    Status Open() override;
    Status Close() override;
    void RequestUnblock() noexcept override;
    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override;

  private:
    bool Refuses() const;

    ICanFlashTransport& inner_;
    const ICancellationToken& cancellation_;
};

} // namespace fastecu::flash
