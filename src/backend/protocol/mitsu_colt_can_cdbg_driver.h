#pragma once
#include "src/backend/protocol/ican_transport.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace mitsu_colt_can_cdbg
{

class CdbgLogDriver
{
  public:
    explicit CdbgLogDriver(cdbg::ICanTransport& transport) : t_(transport)
    {
    }

    // Runs the session-init + seed/key security handshake + frame
    // configuration + start command for `channels` (RAM pointer + size
    // each). Succeeds once the ECU has been told to start streaming.
    fastecu::Status StartFreeFormLog(const std::vector<CdbgChannel>& channels, bytes::Byte instance,
                                     std::uint32_t interval_ms, const fastecu::ICancellationToken& cancellation);
    bool IsStreaming() const
    {
        return streaming_;
    }

    struct PollResult
    {
        bool responded = false;
        std::vector<std::uint32_t> values;
        std::size_t Size() const
        {
            return values.size();
        }
        bool Empty() const
        {
            return values.empty();
        }
        std::uint32_t At(std::size_t index) const
        {
            return values.at(index);
        }
    };

    // Reads at most one streamed frame and reports whether a usable frame was
    // actually received. Cached values are returned only with responded=true.
    fastecu::Result<PollResult> PollOnce(std::chrono::milliseconds timeout,
                                         const fastecu::ICancellationToken& cancellation);

  private:
    cdbg::ICanTransport& t_;
    std::vector<std::vector<CdbgChannel>> frames_;
    std::vector<std::uint32_t> last_values_;
    bool streaming_ = false;
};

} // namespace mitsu_colt_can_cdbg
