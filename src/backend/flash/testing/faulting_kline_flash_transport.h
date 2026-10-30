#pragma once
#include "src/backend/flash/testing/scripted_kline_flash_transport.h"

#include <chrono>
#include <cstddef>
#include <optional>

namespace fastecu::flash
{

enum class KlineFaultOp
{
    kWrite,
    kRead,
    kSetBaud,
    kHeaderMode,
    kDisableLecLines,
    kPulseLec2Line,
};

// A scripted K-Line transport that fails the `fail_at`-th call (1-based) of
// `op` with kDisconnected "injected"; every other call follows the script.
// With `fail_at` 0 it only counts `op` calls, so a clean run sizes a sweep.
class FaultingKlineFlashTransport final : public ScriptedKlineFlashTransport
{
  public:
    using ScriptedKlineFlashTransport::ScriptedKlineFlashTransport;

    KlineFaultOp op = KlineFaultOp::kWrite;
    std::size_t fail_at = 0;
    std::size_t calls = 0;
    // WritesConsumed() when the fault fired; equal to it after the run when
    // nothing was written past the fault.
    std::optional<std::size_t> writes_at_fault;

    Result<std::size_t> Write(bytes::ByteView data) override
    {
        if (Fault(KlineFaultOp::kWrite))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::Write(data);
    }
    Result<OptionalBytes> Read(std::chrono::milliseconds timeout, const ICancellationToken& cancellation) override
    {
        if (Fault(KlineFaultOp::kRead))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::Read(timeout, cancellation);
    }
    Status SetBaud(int baud) override
    {
        if (Fault(KlineFaultOp::kSetBaud))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::SetBaud(baud);
    }
    Status SetAddIso14230Header(bool add_header) override
    {
        if (Fault(KlineFaultOp::kHeaderMode))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::SetAddIso14230Header(add_header);
    }
    Status DisableLecLines() override
    {
        if (Fault(KlineFaultOp::kDisableLecLines))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::DisableLecLines();
    }
    Status PulseLec2Line(std::chrono::milliseconds timeout) override
    {
        if (Fault(KlineFaultOp::kPulseLec2Line))
        {
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedKlineFlashTransport::PulseLec2Line(timeout);
    }

  private:
    bool Fault(KlineFaultOp called)
    {
        if (called != op || ++calls != fail_at)
        {
            return false;
        }
        writes_at_fault = WritesConsumed();
        return true;
    }
};

} // namespace fastecu::flash
