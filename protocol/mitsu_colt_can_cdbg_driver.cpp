#include "protocol/mitsu_colt_can_cdbg_driver.h"

namespace MitsuColtCanCdbg {

namespace {
bool sendAndReceive(cdbg::ICanTransport &t, bytes::ByteView cmd, bytes::Bytes &outReply)
{
    t.write(kRequestCanId, cmd);
    std::uint32_t id = 0;
    outReply = t.read(250, id);
    return !outReply.empty() && id == kReplyCanId;
}
}

bool CdbgLogDriver::startFreeFormLog(const QVector<CdbgChannel> &channels,
                                      bytes::Byte instance, std::uint32_t intervalMs,
                                      QString *errorOut)
{
    streaming_ = false;
    frames_.clear();
    lastValues_.clear();

    if (channels.isEmpty()) {
        if (errorOut)
            *errorOut = "no CDBG log parameters selected";
        return false;
    }

    for (const CdbgChannel &ch : channels) {
        if (ch.size != 1 && ch.size != 2 && ch.size != 4) {
            if (errorOut)
                *errorOut = "CDBG log parameter has unsupported byte length";
            return false;
        }
    }

    bytes::Bytes reply;
    if (!sendAndReceive(t_, buildInitFrame(), reply)) {
        if (errorOut)
            *errorOut = "CDBG session init failed";
        return false;
    }

    if (!sendAndReceive(t_, buildSecuritySeedRequestFrame(), reply)) {
        if (errorOut)
            *errorOut = "CDBG security seed request failed";
        return false;
    }
    std::uint32_t key = seedToKey(extractSeed(reply));
    if (!sendAndReceive(t_, buildSecurityKeyFrame(key), reply)) {
        if (errorOut)
            *errorOut = "CDBG security key request failed";
        return false;
    }
    if (!securityGranted(reply)) {
        if (errorOut)
            *errorOut = "CDBG security access denied";
        return false;
    }

    if (!batchChannelsIntoFrames(channels, frames_)) {
        if (errorOut)
            *errorOut = "too many CDBG log parameters selected";
        return false;
    }

    if (!sendAndReceive(t_, buildLogResetFrame(instance), reply)) {
        if (errorOut)
            *errorOut = "CDBG log reset failed";
        return false;
    }

    for (int f = 0; f < frames_.size(); ++f) {
        const std::vector<CdbgFrame> cmds = buildFrameInitFrames(instance, static_cast<bytes::Byte>(f), frames_.at(f));
        for (const CdbgFrame &cmd : cmds) {
            if (!sendAndReceive(t_, cmd, reply)) {
                if (errorOut)
                    *errorOut = "CDBG log frame setup failed";
                return false;
            }
        }
    }

    if (!sendAndReceive(t_, buildLogStartFrame(instance, static_cast<bytes::Byte>(frames_.size()), intervalMs), reply)) {
        if (errorOut)
            *errorOut = "CDBG log start failed";
        return false;
    }

    int totalChannels = 0;
    for (const auto &frame : frames_)
        totalChannels += frame.size();
    lastValues_.fill(0, totalChannels);

    streaming_ = true;
    return true;
}

QVector<std::uint32_t> CdbgLogDriver::pollOnce(int timeoutMs)
{
    if (!streaming_)
        return {};

    std::uint32_t id = 0;
    const bytes::Bytes frame = t_.read(timeoutMs, id);
    if (!frame.empty() && id == kReplyCanId) {
        bytes::Byte frameIdx = frame[0];
        if (frameIdx < static_cast<bytes::Byte>(frames_.size())) {
            QVector<std::uint32_t> decoded = decodeFrame(frameIdx, frames_.at(frameIdx), frame);
            if (!decoded.isEmpty()) {
                int offset = 0;
                for (int f = 0; f < frameIdx; ++f)
                    offset += frames_.at(f).size();
                for (int i = 0; i < decoded.size(); ++i)
                    lastValues_[offset + i] = decoded.at(i);
            }
        }
    }
    return lastValues_;
}

} // namespace MitsuColtCanCdbg
