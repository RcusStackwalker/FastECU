#include "src/platform/desktop/common/logging/logging_snapshot_adapter.h"

#include <QStringList>
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

fastecu::Result<QString> effective_protocol_filter(fastecu::logging::LoggingProtocolId protocol,
                                                   const QString& protocol_filter)
{
    switch (protocol)
    {
    case fastecu::logging::LoggingProtocolId::kSsm:
        if (protocol_filter.isEmpty())
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "SSM logging protocol filter is empty");
        }
        return protocol_filter;
    case fastecu::logging::LoggingProtocolId::kMutDma:
        return QStringLiteral("MUT_DMA");
    case fastecu::logging::LoggingProtocolId::kCdbg:
        return QStringLiteral("CDBG");
    }
    return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging protocol");
}

fastecu::Result<fastecu::logging::LoggingChannel>
channel_from_parameter(const fastecu::logging::LoggerParameter& parameter, fastecu::logging::LoggingProtocolId protocol)
{
    const auto& conversions = parameter.conversions;
    if (conversions.empty() || conversions.at(0).units.empty() || conversions.at(0).expr.empty() ||
        conversions.at(0).format.empty())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "malformed logging conversion");
    }

    bool address_ok = false;
    const uint address = QString::fromStdString(parameter.address).toUInt(&address_ok, 16);
    bool length_ok = false;
    const uint length = QString::fromStdString(parameter.length).toUInt(&length_ok);
    if (!address_ok || !length_ok)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "invalid logging address or length");
    }

    const QStringList format_fields = QString::fromStdString(conversions.at(0).format).split('.');
    const auto precision = static_cast<int>(format_fields.size() > 1 ? format_fields.at(1).count('0') : 0);
    if (precision > std::numeric_limits<std::uint8_t>::max())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "logging conversion precision is too large");
    }

    return fastecu::logging::LoggingChannel{
        .id = parameter.id,
        .address = static_cast<std::uint32_t>(address),
        .length = static_cast<std::size_t>(length),
        .raw_assembly = protocol == fastecu::logging::LoggingProtocolId::kSsm
                            ? fastecu::logging::RawAssembly::kDecimalBytesConcatenated
                            : fastecu::logging::RawAssembly::kUnsignedIntegerDecimal,
        .from_byte_expression = conversions.at(0).expr,
        .unit = conversions.at(0).units,
        .decimal_precision = static_cast<std::uint8_t>(precision),
    };
}

} // namespace

fastecu::Result<DesktopLoggingSnapshot> make_desktop_logging_snapshot(const fastecu::logging::LoggerModel& model,
                                                                      fastecu::logging::LoggingProtocolId protocol,
                                                                      const QString& protocol_filter,
                                                                      fastecu::logging::LoggingPolicy policy)
{
    const auto filter = effective_protocol_filter(protocol, protocol_filter);
    if (!filter.has_value())
    {
        return std::unexpected(filter.error());
    }
    const auto key = filter->toStdString();
    std::vector<fastecu::logging::LoggingChannel> channels;
    std::vector<std::size_t> response_offsets;
    std::unordered_map<std::string, fastecu::logging::LoggerIdentity> identities;
    std::unordered_set<std::string> enabled_ids;
    const auto& selected_ids = model.Selection().lower_panel_ids;
    for (std::size_t slot = 0; slot < selected_ids.size(); ++slot)
    {
        const auto& id = selected_ids[slot];
        const fastecu::logging::LoggerParameter *selected = nullptr;
        for (const auto& p : model.Definition().parameters)
        {
            if (p.id != id || p.protocol != key ||
                (protocol == fastecu::logging::LoggingProtocolId::kMutDma && !model.ParameterSupported(key, id)))
            {
                continue;
            }
            if (selected != nullptr)
            {
                return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig,
                                     "duplicate logging value id in selected protocol");
            }
            selected = &p;
        }
        if (selected == nullptr)
        {
            continue;
        }
        auto channel = channel_from_parameter(*selected, protocol);
        if (!channel.has_value())
        {
            return std::unexpected(channel.error());
        }
        if (!identities.emplace(id, fastecu::logging::LoggerIdentity{key, id}).second)
        {
            return fastecu::Fail(fastecu::ErrorKind::kInvalidConfig, "duplicate lower-panel logging value id");
        }
        if (protocol != fastecu::logging::LoggingProtocolId::kSsm || model.ParameterSupported(key, id))
        {
            enabled_ids.insert(id);
        }
        if (protocol == fastecu::logging::LoggingProtocolId::kSsm)
        {
            response_offsets.push_back(slot);
        }
        channels.push_back(std::move(*channel));
    }
    auto session = fastecu::logging::MakeLoggingSession(protocol, std::move(channels), policy);
    if (!session.has_value())
    {
        return std::unexpected(session.error());
    }
    return DesktopLoggingSnapshot{
        .session = std::move(*session),
        .response_offsets = std::move(response_offsets),
        .protocol = key,
        .selection = model.Selection(),
        .identities_by_id = std::move(identities),
        .enabled_ids = std::move(enabled_ids),
    };
}

} // namespace fastecu::desktop::logging
