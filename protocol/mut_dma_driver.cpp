#include "protocol/mut_dma_driver.h"
#include "protocol/mut_dma_codec.h"
namespace mutdma {
static bool isAck(const QByteArray& f) { return verifyFrame(f); }
bool MutDmaDriver::startFreeFormLog(const QVector<Channel>& channels,
                                    quint8 setupCmd, quint8 listCmd) {
    channels_ = channels; streaming_ = false;
    if (!init_.wake(t_)) return false;
    t_.write(buildSetupFrame(setupCmd, quint8(channels.size())));
    if (!isAck(t_.read(50, FRAME_LEN))) return false;       // ACK-1
    t_.write(buildIdListFrame(listCmd, channels));
    if (!isAck(t_.read(50, FRAME_LEN))) return false;       // ACK-2
    streaming_ = true;
    return true;
}
QVector<quint32> MutDmaDriver::pollOnce(int timeoutMs) {
    const int want = 1 + responseDataLength(channels_) + 2; // id + data + csum + trailer
    QByteArray fr = t_.read(timeoutMs, want);
    StreamFrame s = parseStreamFrame(fr);
    if (!s.ok) return {};
    return decodeStreamValues(channels_, s.data);
}
}
