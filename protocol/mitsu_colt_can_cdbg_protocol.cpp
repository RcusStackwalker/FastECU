#include "protocol/mitsu_colt_can_cdbg_protocol.h"

namespace MitsuColtCanCdbg {

QByteArray buildInitFrame()
{
    QByteArray f;
    f.append(char(kCmdInit));
    f.append(char(1));
    for (int i = 0; i < 6; ++i) f.append(char(0));
    return f;
}

QByteArray buildSecuritySeedRequestFrame()
{
    QByteArray f;
    f.append(char(kCmdSecuritySeed));
    f.append(char(0));
    f.append(char(kSecurityLogAccess));
    for (int i = 0; i < 5; ++i) f.append(char(0));
    return f;
}

quint32 seedToKey(quint32 seed)
{
    quint8 data[4] = {
        quint8((seed >> 24) & 0xFF),
        quint8((seed >> 16) & 0xFF),
        quint8((seed >> 8) & 0xFF),
        quint8(seed & 0xFF)
    };

    for (int i = 0; i < 4; ++i) {
        quint8 x = data[i];
        switch (x & 0x03) {
        case 0: x = quint8(x + 145); break;
        case 1: x = quint8(x + 24);  break;
        case 2: x = quint8(x + 211); break;
        case 3: x = quint8(x + 2);   break;
        }
        data[i] = quint8((x << 3) | (x >> 5)); // 8-bit rotate-left by 3
    }

    int parity = (data[0] & 1) + (data[1] & 1) + (data[2] & 1) + (data[3] & 1);
    quint8 n[4];
    switch (parity) {
    case 0: n[0]=data[1]; n[1]=data[3]; n[2]=data[2]; n[3]=data[0]; break;
    case 1: n[0]=data[3]; n[1]=data[2]; n[2]=data[0]; n[3]=data[1]; break;
    case 2: n[0]=data[1]; n[1]=data[2]; n[2]=data[3]; n[3]=data[0]; break;
    case 3: n[0]=data[1]; n[1]=data[0]; n[2]=data[2]; n[3]=data[3]; break;
    default: n[0]=data[2]; n[1]=data[0]; n[2]=data[1]; n[3]=data[3]; break;
    }

    quint16 word0 = quint16(((n[0] << 8) + n[1]) * 3 + n[3] * 8);
    quint16 word1 = quint16(((n[2] << 8) + n[3]) * 5 + n[1] * 8);

    return (quint32(word0 >> 8) << 24) | (quint32(word0 & 0xFF) << 16)
         | (quint32(word1 >> 8) << 8)  |  quint32(word1 & 0xFF);
}

quint32 extractSeed(const QByteArray &reply)
{
    if (reply.size() < 8) return 0;
    return (quint32(quint8(reply.at(4))) << 24) | (quint32(quint8(reply.at(5))) << 16)
         | (quint32(quint8(reply.at(6))) << 8)  |  quint32(quint8(reply.at(7)));
}

QByteArray buildSecurityKeyFrame(quint32 key)
{
    QByteArray f;
    f.append(char(kCmdSecurityKey));
    f.append(char(0));
    f.append(char((key >> 24) & 0xFF));
    f.append(char((key >> 16) & 0xFF));
    f.append(char((key >> 8) & 0xFF));
    f.append(char(key & 0xFF));
    f.append(char(0));
    f.append(char(0));
    return f;
}

bool securityGranted(const QByteArray &reply)
{
    if (reply.size() < 4) return false;
    return quint8(reply.at(3)) != 0;
}

QByteArray buildLogResetFrame(quint8 instance)
{
    QByteArray f;
    f.append(char(kCmdLogReset));
    f.append(char(0));
    f.append(char(instance));
    f.append(char(0));
    f.append(char(0));
    f.append(char(0));
    f.append(char(0x06));
    f.append(char(0x31));
    return f;
}

QByteArray buildLogStartFrame(quint8 instance, quint8 frameCount, quint32 intervalMs)
{
    quint8 unitFlag;
    quint16 encoded;
    if (intervalMs > 65535) {
        unitFlag = 1;
        encoded = quint16(intervalMs / 10);
    } else {
        unitFlag = 0;
        encoded = quint16(intervalMs);
    }

    QByteArray f;
    f.append(char(kCmdLogStart));
    f.append(char(0));
    f.append(char(1));
    f.append(char(instance));
    f.append(char(frameCount));
    f.append(char(unitFlag));
    f.append(char((encoded >> 8) & 0xFF));
    f.append(char(encoded & 0xFF));
    return f;
}

bool batchChannelsIntoFrames(const QVector<CdbgChannel> &channels,
                              QVector<QVector<CdbgChannel>> &outFrames)
{
    if (channels.isEmpty()) return false;

    QVector<QVector<CdbgChannel>> frames;
    QVector<CdbgChannel> current;
    int byteIndex = 1;

    for (const CdbgChannel &ch : channels) {
        if (byteIndex + ch.size > 8) {
            frames.append(current);
            current.clear();
            byteIndex = 1;
        }
        current.append(ch);
        byteIndex += ch.size;
    }
    if (!current.isEmpty())
        frames.append(current);

    if (frames.size() > kMaxFrames)
        return false;

    outFrames = frames;
    return true;
}

QVector<QByteArray> buildFrameInitFrames(quint8 instance, quint8 frameIndex,
                                          const QVector<CdbgChannel> &frameItems)
{
    QVector<QByteArray> out;
    for (int i = 0; i < frameItems.size(); ++i) {
        QByteArray select;
        select.append(char(kCmdLogSelectItem));
        select.append(char(0));
        select.append(char(instance));
        select.append(char(frameIndex));
        select.append(char(i));
        select.append(char(0));
        select.append(char(0));
        select.append(char(0));
        out.append(select);

        const CdbgChannel &ch = frameItems.at(i);
        QByteArray pointer;
        pointer.append(char(kCmdLogSetPointer));
        pointer.append(char(0));
        pointer.append(char(ch.size));
        pointer.append(char(0));
        pointer.append(char((ch.pointer >> 24) & 0xFF));
        pointer.append(char((ch.pointer >> 16) & 0xFF));
        pointer.append(char((ch.pointer >> 8) & 0xFF));
        pointer.append(char(ch.pointer & 0xFF));
        out.append(pointer);
    }
    return out;
}

QVector<quint32> decodeFrame(quint8 expectedFrameIndex,
                              const QVector<CdbgChannel> &frameItems,
                              const QByteArray &frame)
{
    if (frame.size() < 1 || quint8(frame.at(0)) != expectedFrameIndex)
        return {};

    int need = 1;
    for (const CdbgChannel &ch : frameItems)
        need += ch.size;
    if (frame.size() < need)
        return {};

    QVector<quint32> out;
    int offset = 1;
    for (const CdbgChannel &ch : frameItems) {
        quint32 value = 0;
        for (int k = 0; k < ch.size; ++k)
            value = (value << 8) | quint32(quint8(frame.at(offset + k)));
        out.append(value);
        offset += ch.size;
    }
    return out;
}

} // namespace MitsuColtCanCdbg
