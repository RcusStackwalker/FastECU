#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/protocol/ikline_transport.h"
#include "src/backend/protocol/imut_dma_init.h"
#include "src/algorithms/protocol/mut_dma/mut_dma_freeform.h"

#include <QByteArray>

#include <cstdint>

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
    // listCmd 0xA1..0xA4 (rate slot). Returns true once streaming is established.
    bool startFreeFormLog(const QVector<Channel>& channels, bytes::Byte setupCmd, bytes::Byte listCmd);
    bool isStreaming() const
    {
        return streaming_;
    }
    // Read one streamed frame (within timeoutMs) and decode to per-channel values.
    QVector<std::uint32_t> pollOnce(int timeoutMs);
    // Write `bytes` to RAM at `addr` via 0x87 sub-cmd 3 (chunked). Returns true if
    // every chunk gets a valid echo response. Caller must validate the address range.
    bool writeMemory(std::uint16_t addr, bytes::ByteView bytes);
    bool writeMemory(std::uint16_t addr, const QByteArray& bytes);
    void setChannelsForTest(const QVector<Channel>& ch)
    {
        channels_ = ch;
    }

  private:
    IKlineTransport& t_;
    IMutDmaInit& init_;
    QVector<Channel> channels_;
    bool streaming_ = false;
};
} // namespace mutdma
