#pragma once
#include "src/backend/protocol/ican_transport.h"
#include "src/algorithms/protocol/colt/mitsu_colt_can_cdbg_protocol.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <cstdint>
#include <vector>

namespace MitsuColtCanCdbg
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
    fastecu::Status startFreeFormLog(const std::vector<CdbgChannel>& channels,
                                     bytes::Byte instance, std::uint32_t intervalMs,
                                     const fastecu::ICancellationToken& cancellation);
    bool isStreaming() const
    {
        return streaming_;
    }

    // Reads at most one streamed frame (within timeoutMs) and merges any
    // decoded channel values into this driver's per-channel cache. Returns
    // the full per-channel raw value vector (using cached/last-known values
    // for channels whose frame hasn't arrived since streaming started, 0
    // until then), or an empty vector if not currently streaming.
    fastecu::Result<std::vector<std::uint32_t>> pollOnce(
        int timeoutMs, const fastecu::ICancellationToken& cancellation);

  private:
    cdbg::ICanTransport& t_;
    std::vector<std::vector<CdbgChannel>> frames_;
    std::vector<std::uint32_t> lastValues_;
    bool streaming_ = false;
};

} // namespace MitsuColtCanCdbg
