#pragma once
#include "src/backend/protocol/idiagnostic_link.h"

class SerialPortActions;

namespace fastecu::diagnostics
{

// IDiagnosticLink over a non-owning SerialPortActions. The caller keeps the
// facade alive for the link's lifetime; MainWindow's facade outlives every
// diagnostic dialog.
class SerialDiagnosticLink final : public IDiagnosticLink
{
  public:
    explicit SerialDiagnosticLink(SerialPortActions *serial) : serial_(serial)
    {
    }

    Status Open(const KlineLinkConfig& config) override;
    Status Open(const CanLinkConfig& config) override;
    Status Reset() override;
    Status SetHeader(KlineHeader header) override;
    Status SetP1Max(std::chrono::milliseconds p1_max) override;
    Result<bytes::Bytes> FiveBaudInit(std::uint8_t address) override;
    Status FastInit(bytes::ByteView wakeup) override;
    Result<bytes::Bytes> Write(bytes::ByteView data) override;
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    Result<OptionalBytes> ReadObd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    bool UsesJ2534() const override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::diagnostics
