#include "protocol/mut_dma_driver.h"
#include "protocol/mut_dma_codec.h"
#include "protocol/mut_dma_memory.h"
#include "protocol/qt_bytes.h"
namespace mutdma {
static bool ackOk(const QByteArray& f, quint8 cA, quint8 cB) {
    return verifyFrame(f) && (quint8(f.at(0)) == cA || quint8(f.at(0)) == cB);
}
bool MutDmaDriver::startFreeFormLog(const QVector<Channel>& channels,
                                    quint8 setupCmd, quint8 listCmd) {
    channels_ = channels; streaming_ = false;
    if (!init_.wake(t_)) return false;
    t_.write(bytes::view(buildSetupFrame(setupCmd, quint8(channels.size()))));
    QByteArray resp1 = bytes::toQByteArray(t_.read(50, FRAME_LEN));
    if (!ackOk(resp1, 0xA5, 0xB5)) return false;           // ACK-1
    t_.write(bytes::view(buildIdListFrame(listCmd, channels)));
    QByteArray resp2 = bytes::toQByteArray(t_.read(50, FRAME_LEN));
    if (!ackOk(resp2, 0x05, 0x15)) return false;           // ACK-2
    streaming_ = true;
    return true;
}
bool MutDmaDriver::writeMemory(quint16 addr, const QByteArray& bytes) {
    if (!init_.wake(t_)) return false;
    const QVector<QByteArray> frames = buildWriteFrames(addr, bytes);
    if (frames.isEmpty() && !bytes.isEmpty()) return false;
    for (const QByteArray& f : frames) {
        t_.write(bytes::view(f));
        if (!verifyFrame(bytes::toQByteArray(t_.read(50, FRAME_LEN)))) return false;  // echo ack
    }
    return true;
}
QVector<quint32> MutDmaDriver::pollOnce(int timeoutMs) {
    const int want = 1 + responseDataLength(channels_) + 2; // id + data + csum + trailer
    QByteArray fr = bytes::toQByteArray(t_.read(timeoutMs, want));
    StreamFrame s = parseStreamFrame(fr);
    if (!s.ok) return {};
    return decodeStreamValues(channels_, s.data);
}
}
