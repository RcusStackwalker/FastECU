#include "src/backend/logging/protocols/cdbg_logging_protocol.h"
#include "src/backend/logging/romraider_conversion.h"
#include "src/backend/protocol/transport_legacy_compat.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::vector<fastecu::logging::LoggingChannel> channelsFromLogValues(
    FileActions::LogValuesStructure *lv, QVector<int>& outIndices)
{
    std::vector<fastecu::logging::LoggingChannel> channels;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j) && lv->log_value_protocol.at(j) == "CDBG")
            {
                channels.push_back(fastecu::logging::LoggingChannel{
                    .id = lv->log_value_id.at(j).toStdString(),
                    .address = lv->log_value_address.at(j).toUInt(nullptr, 16),
                    .length = static_cast<std::size_t>(lv->log_value_length.at(j).toUInt()),
                    .raw_assembly = fastecu::logging::RawAssembly::UnsignedIntegerDecimal,
                    .from_byte_expression = "x",
                    .unit = "",
                    .decimal_precision = 0,
                });
                outIndices.append(j);
            }
        }
    }
    return channels;
}
} // namespace

CdbgLoggingProtocol::CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                                         FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : logValues_(logValues), fileActions_(fileActions)
{
    // Transitional signature retained until the Qt worker is removed in Task 7.
    // Raw-CAN setup/open is platform-owned; this wrapper intentionally ignores it.
    (void)serial;
    auto channels = channelsFromLogValues(logValues_, channelLogValueIndex_);
    core_ = std::make_unique<fastecu::logging::CdbgLoggingProtocol>(
        std::move(transport), std::move(channels));
}

fastecu::Status CdbgLoggingProtocol::start()
{
    return core_->start(
        fastecu::transport_legacy_compat::detail::never_cancelled());
}

fastecu::Result<PollData> CdbgLoggingProtocol::poll(int timeoutMs)
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
        result->samples.size(), static_cast<std::size_t>(channelLogValueIndex_.size()));
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        const int j = channelLogValueIndex_.at(static_cast<qsizetype>(i));
        const QString value = QString::fromStdString(result->samples[i].raw_value);
        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
        data.samples.append(LogSample{j, calc_value});
    }

    return data;
}

void CdbgLoggingProtocol::stop()
{
    (void)core_->stop();
}
