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
// transport. The critical-section depth is touched by the worker thread only.
class GuardedCanFlashTransport final : public ICanFlashTransport
{
  public:
    GuardedCanFlashTransport(ICanFlashTransport& inner, const ICancellationToken& cancellation);

    // While one is alive, Write/Read do not stop on cancellation, so a
    // multi-message sequence that must not be torn can finish. The first
    // Write/Read after the last section ends reports kCancelled. Sections
    // nest. Must not outlive the guard that created it.
    class CriticalSection
    {
      public:
        CriticalSection(const CriticalSection&) = delete;
        CriticalSection& operator=(const CriticalSection&) = delete;
        CriticalSection(CriticalSection&& other) noexcept;
        CriticalSection& operator=(CriticalSection&&) = delete;
        ~CriticalSection();

      private:
        friend class GuardedCanFlashTransport;
        explicit CriticalSection(GuardedCanFlashTransport& guard) noexcept;
        GuardedCanFlashTransport *guard_;
    };

    [[nodiscard]] CriticalSection EnterCriticalSection();

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
    int critical_depth_ = 0;
};

} // namespace fastecu::flash
