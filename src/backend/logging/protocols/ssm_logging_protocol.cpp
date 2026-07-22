#include "src/backend/logging/protocols/ssm_logging_protocol.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "src/backend/logging/romraider_conversion.h"
#include "src/backend/protocol/transport_legacy_compat.h"

namespace
{
std::vector<fastecu::logging::LoggingChannel> channelsFromLogValues(
    FileActions::LogValuesStructure *lv, const QString& protocolFilter,
    QVector<int>& outIndices, QVector<bool>& outEnabled,
    std::vector<std::size_t>& outResponseOffsets)
{
    std::unordered_map<std::string, std::vector<int>> logValueIndicesById;
    logValueIndicesById.reserve(
        static_cast<std::size_t>(lv->log_value_id.length()));
    for (int j = 0; j < lv->log_value_id.length(); ++j)
    {
        if (lv->log_value_protocol.at(j) == protocolFilter)
        {
            logValueIndicesById[lv->log_value_id.at(j).toStdString()].push_back(j);
        }
    }

    std::vector<fastecu::logging::LoggingChannel> channels;
    outIndices.clear();
    outEnabled.clear();
    outResponseOffsets.clear();
    channels.reserve(static_cast<std::size_t>(lv->lower_panel_log_value_id.length()));
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); ++i)
    {
        const auto it = logValueIndicesById.find(
            lv->lower_panel_log_value_id.at(i).toStdString());
        if (it == logValueIndicesById.end())
        {
            continue;
        }

        for (int j : it->second)
        {
            channels.push_back(fastecu::logging::LoggingChannel{
                .id = lv->log_value_id.at(j).toStdString(),
                .address = lv->log_value_address.at(j).toUInt(nullptr, 16),
                .length = static_cast<std::size_t>(
                    lv->log_value_length.at(j).toUInt()),
                .raw_assembly =
                    fastecu::logging::RawAssembly::DecimalBytesConcatenated,
                .from_byte_expression = "x",
                .unit = "",
                .decimal_precision = 0,
            });
            outIndices.append(j);
            outEnabled.append(lv->log_value_enabled.at(j) == "1");
            outResponseOffsets.push_back(static_cast<std::size_t>(i));
        }
    }
    return channels;
}
} // namespace

SsmLoggingProtocol::SsmLoggingProtocol(
    fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
    FileActions::LogValuesStructure *logValues, FileActions *fileActions,
    QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter)
    : logValues_(logValues), fileActions_(fileActions)
{
    std::vector<std::size_t> responseOffsets;
    auto channels = channelsFromLogValues(
        logValues_, logValueProtocolFilter, channelLogValueIndex_, channelEnabled_,
        responseOffsets);
    core_ = std::make_unique<fastecu::logging::SsmLoggingProtocol>(
        clock, std::move(transport), std::move(channels),
        std::move(responseOffsets), targetIsEcu, useOpenport2Adapter);
}

fastecu::Status SsmLoggingProtocol::start()
{
    return core_->start(
        fastecu::transport_legacy_compat::detail::never_cancelled());
}

fastecu::Result<PollData> SsmLoggingProtocol::poll(int timeoutMs)
{
    auto result = core_->poll(
        timeoutMs, fastecu::transport_legacy_compat::detail::never_cancelled());
    if (!result)
    {
        return std::unexpected(result.error());
    }

    PollData data;
    data.responded = result->responded;
    const auto sampleCount = std::min(
        result->samples.size(),
        static_cast<std::size_t>(channelLogValueIndex_.size()));
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        const auto qtIndex = static_cast<qsizetype>(i);
        if (!channelEnabled_.at(qtIndex))
        {
            continue;
        }
        const int j = channelLogValueIndex_.at(qtIndex);
        const QString value = QString::fromStdString(result->samples[i].raw_value);
        const QString calcValue =
            convertRomRaiderValue(fileActions_, logValues_, j, value);
        data.samples.append(LogSample{j, calcValue});
    }
    return data;
}

void SsmLoggingProtocol::stop()
{
    (void)core_->stop();
}
