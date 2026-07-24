#pragma once
#include <atomic>
#include <memory>
#include <optional>

#include "src/backend/flash/flash_executor.h"

class SerialPortActions;

namespace fastecu::flash
{

// Owns its SerialPortActions instance so close() has a well-defined
// teardown trigger -- see DesktopKlineFlashTransport's header comment
// (this package) for the rationale: SerialPortActions exposes no
// close()/disconnect(), only a destructor.
class DesktopCanFlashTransport final : public ICanFlashTransport
{
  public:
    explicit DesktopCanFlashTransport(std::unique_ptr<SerialPortActions> serial);
    ~DesktopCanFlashTransport() override;

    Status configure(const Iso15765Config& config) override;
    Status open() override;
    Status close() override;
    void request_unblock() noexcept override;

    Status write(bytes::ByteView data, const ICancellationToken& cancellation) override;
    Result<std::optional<bytes::Bytes>> read(int timeout_ms,
                                             const ICancellationToken& cancellation) override;

  private:
    std::unique_ptr<SerialPortActions> serial_;

    // Best-effort cancellation: SerialPortActions exposes no interrupt
    // primitive, so this can only be checked before the *next* read/write
    // call issues -- an already in-flight read_serial_data(timeout) call
    // still returns via its own existing bounded timeout, not instantly.
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
