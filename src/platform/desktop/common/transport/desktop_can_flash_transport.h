#pragma once
#include <atomic>
#include <memory>
#include <optional>

#include "src/backend/flash/flash_executor.h"

class SerialPortActions;

namespace fastecu::flash
{

// Owning constructor: has its own well-defined teardown trigger -- see
// DesktopKlineFlashTransport's header comment (this package) for the
// rationale: SerialPortActions exposes no close()/disconnect(), only a
// destructor.
//
// Non-owning constructor (added Task 17): same rationale as
// DesktopKlineFlashTransport's sibling constructor -- required so this
// transport can wrap MainWindow's single, session-lifetime SerialPortActions
// instance without close() destroying it out from under the rest of the
// application. See DesktopKlineFlashTransport.h's non-owning constructor
// comment for the full explanation. Callers using this constructor must
// keep the pointed-to SerialPortActions alive for at least this transport's
// lifetime.
class DesktopCanFlashTransport final : public ICanFlashTransport
{
  public:
    explicit DesktopCanFlashTransport(std::unique_ptr<SerialPortActions> serial);
    explicit DesktopCanFlashTransport(SerialPortActions *serial);
    ~DesktopCanFlashTransport() override;

    Status configure(const Iso15765Config& config) override;
    Status open() override;
    Status close() override;
    void request_unblock() noexcept override;

    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> read(int timeout_ms, const ICancellationToken& cancellation) override;

  private:
    // Null when constructed from the non-owning (raw pointer) constructor;
    // close() only ever resets THIS, never touches serial_ directly -- see
    // DesktopKlineFlashTransport.h for the full rationale.
    std::unique_ptr<SerialPortActions> owned_serial_;
    SerialPortActions *serial_ = nullptr;

    // Best-effort cancellation: SerialPortActions exposes no interrupt
    // primitive, so this can only be checked before the *next* read/write
    // call issues -- an already in-flight read_serial_data(timeout) call
    // still returns via its own existing bounded timeout, not instantly.
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
