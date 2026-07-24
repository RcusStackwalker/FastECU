#pragma once
#include <atomic>
#include <memory>

#include "src/backend/flash/flash_executor.h"

class SerialPortActions;

namespace fastecu::flash
{

// Owns its SerialPortActions instance so close() has a well-defined
// teardown trigger -- SerialPortActions itself exposes no close()/
// disconnect() (confirmed absent from both serial_port_actions.h and
// serial_port_actions.cpp, step 5c Task 12); its destructor is the only
// teardown path, and it already drains in-flight backend calls before
// joining its I/O thread. Constructing this adapter with the instance it
// should own, then having close() reset() it, gives that destructor a
// well-defined trigger.
class DesktopKlineFlashTransport final : public IKlineFlashTransport
{
  public:
    explicit DesktopKlineFlashTransport(std::unique_ptr<SerialPortActions> serial);
    ~DesktopKlineFlashTransport() override;

    Status configure(const KlineConfig& config) override;
    Status open() override;
    Status close() override;
    void request_unblock() noexcept override;

    Status setBaud(int baud) override;
    Result<std::size_t> write(bytes::ByteView data) override;
    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    std::unique_ptr<SerialPortActions> serial_;

    // Best-effort cancellation: SerialPortActions exposes no interrupt
    // primitive, so this can only be checked before the *next* read/write
    // call issues -- an already in-flight read_serial_data(timeout) call
    // still returns via its own existing bounded timeout, not instantly.
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
