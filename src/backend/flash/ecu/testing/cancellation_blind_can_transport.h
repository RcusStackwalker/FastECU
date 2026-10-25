#pragma once

#include <functional>

#include "src/backend/flash/testing/scripted_can_flash_transport.h"
#include "src/backend/ports/manual_cancellation_token.h"

namespace fastecu::flash::testing
{
// Hooks observe accepted operations while the delegate deliberately ignores the
// attempt's token. Count attempts before delegation so guard bypass is visible.
class CancellationBlindCanTransport final : public ScriptedCanFlashTransport
{
  public:
    Status Write(bytes::ByteView data, const ICancellationToken&) override
    {
        ++write_attempts;
        const auto result = ScriptedCanFlashTransport::Write(data, never_cancelled_);
        if (result.has_value() && on_write)
        {
            on_write(data);
        }
        return result;
    }
    Result<std::optional<bytes::Bytes>> Read(std::chrono::milliseconds timeout, const ICancellationToken&) override
    {
        ++read_attempts;
        const auto result = ScriptedCanFlashTransport::Read(timeout, never_cancelled_);
        if (result.has_value() && result->has_value() && on_read)
        {
            on_read(**result);
        }
        return result;
    }
    int write_attempts = 0;
    int read_attempts = 0;
    std::function<void(bytes::ByteView)> on_write;
    std::function<void(bytes::ByteView)> on_read;

  private:
    ManualCancellationToken never_cancelled_;
};
} // namespace fastecu::flash::testing
