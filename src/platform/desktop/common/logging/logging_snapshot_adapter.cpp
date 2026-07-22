#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace fastecu::desktop::logging
{
namespace
{

fastecu::Result<QString> effective_protocol_filter(
    fastecu::logging::LoggingProtocolId protocol, const QString& protocol_filter)
{
    switch (protocol)
    {
    case fastecu::logging::LoggingProtocolId::Ssm:
        if (protocol_filter.isEmpty())
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "SSM logging protocol filter is empty");
        }
        return protocol_filter;
    case fastecu::logging::LoggingProtocolId::MutDma:
        return QStringLiteral("MUT_DMA");
    case fastecu::logging::LoggingProtocolId::Cdbg:
        return QStringLiteral("CDBG");
    }
    return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "invalid logging protocol");
}

bool selected_by_protocol(fastecu::logging::LoggingProtocolId protocol, const QString& enabled)
{
    // Legacy SSM polls every lower-panel slot to preserve response offsets,
    // while MUT/DMA excludes disabled values.  CDBG has no enabled filter.
    return protocol != fastecu::logging::LoggingProtocolId::MutDma || enabled == "1";
}

fastecu::Result<fastecu::logging::LoggingChannel> channel_from_legacy_row(
    const FileActions::LogValuesStructure& log_values, int row,
    fastecu::logging::LoggingProtocolId protocol)
{
    const QStringList unit_fields = log_values.log_value_units.at(row).split(',');
    if (unit_fields.size() < 4 || unit_fields.at(1).isEmpty() || unit_fields.at(2).isEmpty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "malformed logging conversion");
    }

    bool address_ok = false;
    const uint address = log_values.log_value_address.at(row).toUInt(&address_ok, 16);
    bool length_ok = false;
    const uint length = log_values.log_value_length.at(row).toUInt(&length_ok);
    if (!address_ok || !length_ok)
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "invalid logging address or length");
    }

    const QStringList format_fields = unit_fields.at(3).split('.');
    const int precision = format_fields.size() > 1 ? format_fields.at(1).count('0') : 0;
    if (precision > std::numeric_limits<std::uint8_t>::max())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "logging conversion precision is too large");
    }

    return fastecu::logging::LoggingChannel{
        .id = log_values.log_value_id.at(row).toStdString(),
        .address = static_cast<std::uint32_t>(address),
        .length = static_cast<std::size_t>(length),
        .raw_assembly = protocol == fastecu::logging::LoggingProtocolId::Ssm
                            ? fastecu::logging::RawAssembly::DecimalBytesConcatenated
                            : fastecu::logging::RawAssembly::UnsignedIntegerDecimal,
        .from_byte_expression = unit_fields.at(2).toStdString(),
        .unit = unit_fields.at(1).toStdString(),
        .decimal_precision = static_cast<std::uint8_t>(precision),
    };
}

} // namespace

fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(
    const FileActions::LogValuesStructure& log_values,
    fastecu::logging::LoggingProtocolId protocol, const QString& protocol_filter,
    fastecu::logging::LoggingPolicy policy)
{
    if (!FileActions::validate_logger_values(log_values))
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                             "malformed legacy logging value lists");
    }

    auto filter = effective_protocol_filter(protocol, protocol_filter);
    if (!filter)
    {
        return std::unexpected(filter.error());
    }

    std::vector<fastecu::logging::LoggingChannel> channels;
    std::vector<std::size_t> response_offsets;
    std::unordered_map<std::string, int> selected_indices_by_id;
    std::unordered_set<std::string> enabled_ids;
    channels.reserve(static_cast<std::size_t>(log_values.lower_panel_log_value_id.size()));
    response_offsets.reserve(static_cast<std::size_t>(log_values.lower_panel_log_value_id.size()));
    selected_indices_by_id.reserve(
        static_cast<std::size_t>(log_values.lower_panel_log_value_id.size()));
    enabled_ids.reserve(static_cast<std::size_t>(log_values.lower_panel_log_value_id.size()));

    for (qsizetype lower_panel_index = 0;
         lower_panel_index < log_values.lower_panel_log_value_id.size(); ++lower_panel_index)
    {
        const QString& lower_panel_id =
            log_values.lower_panel_log_value_id.at(lower_panel_index);
        int selected_row = -1;
        for (int row = 0; row < log_values.log_value_id.size(); ++row)
        {
            if (log_values.log_value_id.at(row) != lower_panel_id ||
                log_values.log_value_protocol.at(row) != *filter ||
                !selected_by_protocol(protocol, log_values.log_value_enabled.at(row)))
            {
                continue;
            }
            if (selected_row >= 0)
            {
                return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                     "duplicate logging value id in selected protocol");
            }
            selected_row = row;
        }
        if (selected_row < 0)
        {
            continue;
        }

        auto channel = channel_from_legacy_row(log_values, selected_row, protocol);
        if (!channel)
        {
            return std::unexpected(channel.error());
        }
        if (!selected_indices_by_id.emplace(channel->id, selected_row).second)
        {
            return fastecu::fail(fastecu::ErrorKind::InvalidConfig,
                                 "duplicate lower-panel logging value id");
        }
        if (protocol != fastecu::logging::LoggingProtocolId::Ssm ||
            log_values.log_value_enabled.at(selected_row) == "1")
        {
            enabled_ids.insert(channel->id);
        }
        if (protocol == fastecu::logging::LoggingProtocolId::Ssm)
        {
            response_offsets.push_back(static_cast<std::size_t>(lower_panel_index));
        }
        channels.push_back(std::move(*channel));
    }

    auto session = fastecu::logging::make_logging_session(protocol, std::move(channels), policy);
    if (!session)
    {
        return std::unexpected(session.error());
    }
    return DesktopLoggingSnapshot{
        .session = std::move(*session),
        .response_offsets = std::move(response_offsets),
        .index_by_id = std::move(selected_indices_by_id),
        .enabled_ids = std::move(enabled_ids),
    };
}

} // namespace fastecu::desktop::logging
