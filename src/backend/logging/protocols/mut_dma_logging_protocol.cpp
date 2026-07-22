#include "src/backend/logging/protocols/mut_dma_logging_protocol.h"
#include "src/backend/logging/romraider_conversion.h"

#include <cstdint>

using namespace mutdma;

namespace
{
// Build the free-form channel list from the enabled MUT_DMA log values, in the
// same order the display path consumes them. outIndices[i] is the index j into
// logValues->log_value_* for the channel returned at position i.
QVector<Channel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int>& outIndices)
{
    QVector<Channel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j) && lv->log_value_protocol.at(j) == "MUT_DMA" && lv->log_value_enabled.at(j) == "1")
            {
                Channel c{};
                c.id = static_cast<std::uint16_t>(lv->log_value_address.at(j).toUInt(nullptr, 16));
                c.len = static_cast<bytes::Byte>(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}
} // namespace

MutDmaLoggingProtocol::MutDmaLoggingProtocol(std::unique_ptr<IKlineTransport> transport,
                                             std::unique_ptr<IMutDmaInit> init,
                                             FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport)), init_(std::move(init)), logValues_(logValues), fileActions_(fileActions), driver_(*transport_, *init_)
{
}

fastecu::Status MutDmaLoggingProtocol::start()
{
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }

    QVector<Channel> channels = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (!driver_.startFreeFormLog(channels, 0xA0, 0xA1))
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse, "MUT/DMA free-form handshake failed");
    }
    return {};
}

fastecu::Result<PollData> MutDmaLoggingProtocol::poll(int timeoutMs)
{
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }
    if (!driver_.isStreaming())
    {
        return PollData{false, {}};
    }

    QVector<std::uint32_t> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty())
    {
        return PollData{false, {}};
    }

    PollData data;
    data.responded = true;
    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i)
    {
        int j = channelLogValueIndex_.at(i);
        QString value = QString::number(vals.at(i));
        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
        data.samples.append(LogSample{j, calc_value});
    }

    return data;
}

void MutDmaLoggingProtocol::stop()
{
    // MutDmaDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
