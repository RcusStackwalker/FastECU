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

    Status open(const KlineLinkConfig& config) override;
    Status open(const CanLinkConfig& config) override;
    Status reset() override;
    Status set_header(KlineHeader header) override;
    Status set_p1_max(std::chrono::milliseconds p1_max) override;
    Result<bytes::Bytes> five_baud_init(std::uint8_t address) override;
    Status fast_init(bytes::ByteView wakeup) override;
    Result<bytes::Bytes> write(bytes::ByteView data) override;
    Result<OptionalBytes> read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    Result<OptionalBytes> read_obd(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override;
    bool uses_j2534() const override;

  private:
    SerialPortActions *serial_;
};

} // namespace fastecu::diagnostics
