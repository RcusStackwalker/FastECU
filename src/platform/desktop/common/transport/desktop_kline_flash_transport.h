#pragma once
#include <atomic>
#include <memory>

#include "src/backend/flash/flash_executor.h"

class SerialPortActions;

namespace fastecu::flash
{

// Owning constructor: has its own well-defined teardown trigger --
// SerialPortActions itself exposes no close()/disconnect() (confirmed
// absent from both serial_port_actions.h and serial_port_actions.cpp, step
// 5c Task 12); its destructor is the only teardown path, and it already
// drains in-flight backend calls before joining its I/O thread. Owning
// construction gives close() a destructor to trigger.
//
// Non-owning constructor (added Task 17): required for MainWindow's single,
// session-lifetime SerialPortActions instance (mainwindow.h/.cpp:
// `serial = new SerialPortActions(...)`, constructed once and reused across
// every read/write/flash/logging operation for the app's entire lifetime,
// including every EEPROM dialog attempt). The owning constructor's
// close() == serial_.reset() would DESTROY that shared instance the moment
// any one FlashWorker attempt finishes (success or failure) -- breaking
// every subsequent operation for the rest of the session. The non-owning
// constructor stores only a raw, externally-owned pointer; close() drops
// this adapter's reference to it (so post-close() calls still correctly
// fail with Disconnected, matching the owning path's contract) without
// invoking its destructor. Callers using this constructor must keep the
// pointed-to SerialPortActions alive for at least this transport's
// lifetime -- trivially true for MainWindow's serial member, which outlives
// every dialog.
class DesktopKlineFlashTransport final : public IKlineFlashTransport
{
  public:
    explicit DesktopKlineFlashTransport(std::unique_ptr<SerialPortActions> serial);
    explicit DesktopKlineFlashTransport(SerialPortActions *serial);
    ~DesktopKlineFlashTransport() override;

    Status configure(const KlineConfig& config) override;
    Status open() override;
    Status close() override;
    Status disable_lec_lines() override;
    Status pulse_lec_2_line(int timeout_ms) override;
    Status enable_programming_voltage_line() override;
    bool requires_post_kernel_upload_delay() const override;
    Status set_add_iso14230_header(bool add_header) override;
    void request_unblock() noexcept override;

    Status setBaud(int baud) override;
    Result<std::size_t> write(bytes::ByteView data) override;
    Result<OptionalBytes> read(int timeout_ms, const ICancellationToken& cancellation) override;
    bool isOpen() const override;

  private:
    // Null when constructed from the non-owning (raw pointer) constructor;
    // close() only ever resets THIS, never touches serial_ directly, so a
    // non-owning instance's serial_ pointer survives close() while the
    // owning path's underlying object is destroyed exactly as before.
    std::unique_ptr<SerialPortActions> owned_serial_;
    SerialPortActions *serial_ = nullptr;

    // Best-effort cancellation: SerialPortActions exposes no interrupt
    // primitive, so this can only be checked before the *next* read/write
    // call issues -- an already in-flight read_serial_data(timeout) call
    // still returns via its own existing bounded timeout, not instantly.
    std::atomic<bool> unblock_requested_{false};
};

} // namespace fastecu::flash
