#pragma once
#include "protocol/ican_transport.h"
#include "protocol/mitsu_colt_can_cdbg_protocol.h"
#include <QString>
#include <QVector>

#include <cstdint>

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
    // each). Returns true once the ECU has been told to start streaming.
    bool startFreeFormLog(const QVector<CdbgChannel>& channels,
                          bytes::Byte instance = 0, std::uint32_t intervalMs = 10,
                          QString *errorOut = nullptr);
    bool isStreaming() const
    {
        return streaming_;
    }

    // Reads at most one streamed frame (within timeoutMs) and merges any
    // decoded channel values into this driver's per-channel cache. Returns
    // the full per-channel raw value vector (using cached/last-known values
    // for channels whose frame hasn't arrived since streaming started, 0
    // until then), or an empty vector if not currently streaming.
    QVector<std::uint32_t> pollOnce(int timeoutMs);

  private:
    cdbg::ICanTransport& t_;
    QVector<QVector<CdbgChannel>> frames_;
    QVector<std::uint32_t> lastValues_;
    bool streaming_ = false;
};

} // namespace MitsuColtCanCdbg
