#pragma once
#include "src/backend/flash/testing/scripted_can_flash_transport.h"

#include <chrono>
#include <cstddef>
#include <optional>

namespace fastecu::flash
{

// A scripted CAN transport that fails the `fail_write_at`-th write or the
// `fail_read_at`-th read (1-based) with kDisconnected "injected"; every other
// call follows the script. With both at 0 it only counts calls, so a clean
// run sizes a sweep.
class FaultingCanFlashTransport : public ScriptedCanFlashTransport
{
  public:
    using ScriptedCanFlashTransport::ScriptedCanFlashTransport;

    std::size_t fail_write_at = 0;
    std::size_t fail_read_at = 0;
    // WritesConsumed() when the fault fired; equal to it after the run when
    // nothing was written past the fault.
    std::optional<std::size_t> writes_at_fault;

    Status Write(bytes::ByteView data, const ICancellationToken& cancellation) override
    {
        if (++writes_ == fail_write_at)
        {
            writes_at_fault = WritesConsumed();
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedCanFlashTransport::Write(data, cancellation);
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout,
                                             const ICancellationToken& cancellation) override
    {
        if (++reads_ == fail_read_at)
        {
            writes_at_fault = WritesConsumed();
            return Fail(ErrorKind::kDisconnected, "injected");
        }
        return ScriptedCanFlashTransport::Read(timeout, cancellation);
    }
    std::size_t Writes() const
    {
        return writes_;
    }
    std::size_t Reads() const
    {
        return reads_;
    }

  private:
    std::size_t writes_ = 0;
    std::size_t reads_ = 0;
};

} // namespace fastecu::flash
