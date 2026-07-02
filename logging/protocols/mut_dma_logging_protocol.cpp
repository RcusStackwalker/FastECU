#include "logging/protocols/mut_dma_logging_protocol.h"
#include "logging/romraider_conversion.h"

using namespace mutdma;

namespace {
// Build the free-form channel list from the enabled MUT_DMA log values, in the
// same order the display path consumes them. outIndices[i] is the index j into
// logValues->log_value_* for the channel returned at position i.
QVector<Channel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int> &outIndices)
{
    QVector<Channel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < lv->log_value_id.length(); j++) {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "MUT_DMA"
                && lv->log_value_enabled.at(j) == "1") {
                Channel c;
                c.id = quint16(lv->log_value_address.at(j).toUInt(nullptr, 16));
                c.len = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}
}

MutDmaLoggingProtocol::MutDmaLoggingProtocol(std::unique_ptr<IKlineTransport> transport,
                                              std::unique_ptr<IMutDmaInit> init,
                                              FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport))
    , init_(std::move(init))
    , logValues_(logValues)
    , fileActions_(fileActions)
    , driver_(*transport_, *init_)
{
}

bool MutDmaLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    channels_ = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (!driver_.startFreeFormLog(channels_, 0xA0, 0xA1)) {
        if (errorOut) *errorOut = "MUT/DMA free-form handshake failed";
        return false;
    }
    return true;
}

PollResult MutDmaLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }
    if (!driver_.isStreaming()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    QVector<quint32> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i) {
        int j = channelLogValueIndex_.at(i);
        QString value = QString::number(vals.at(i));
        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
        result.samples.append(LogSample{j, calc_value});
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void MutDmaLoggingProtocol::stop()
{
    // MutDmaDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
