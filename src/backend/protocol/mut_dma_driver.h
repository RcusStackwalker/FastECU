#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace mutdma
{
class MutDmaDriver
{
  public:
    MutDmaDriver(IKlineTransport& transport, IMutDmaInit& init)
        : t_(transport), init_(init)
    {
    }
    // Wake + run the free-form handshake for `channels`. setupCmd 0xA0/0xB0,
    // listCmd 0xA1..0xA4 (rate slot). Succeeds once streaming is established.
    fastecu::Status startFreeFormLog(const std::vector<Channel>& channels,
                                     bytes::Byte setupCmd, bytes::Byte listCmd,
                                     const fastecu::ICancellationToken& cancellation);
    bool isStreaming() const
    {
        return streaming_;
    }
    // Read one streamed frame (within timeoutMs) and decode to per-channel values.
    fastecu::Result<std::vector<std::uint32_t>> pollOnce(
        int timeoutMs, const fastecu::ICancellationToken& cancellation);
    // Write `bytes` to RAM at `addr` via 0x87 sub-cmd 3 (chunked). Succeeds if
    // every chunk gets a valid echo response. Caller must validate the address range.
    fastecu::Status writeMemory(std::uint16_t addr, bytes::ByteView data,
                                const fastecu::ICancellationToken& cancellation);
    void setChannelsForTest(std::vector<Channel> ch)
    {
        channels_ = std::move(ch);
        streaming_ = true;
    }

  private:
    IKlineTransport& t_;
    IMutDmaInit& init_;
    std::vector<Channel> channels_;
    bool streaming_ = false;
};
} // namespace mutdma
