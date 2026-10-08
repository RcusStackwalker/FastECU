#include "src/backend/logging/logging_run_snapshot.h"

#include <format>
#include <utility>

#include "src/backend/logging/logging_channel_preparation.h"

namespace fastecu::logging
{
namespace
{
fastecu::Result<std::string> effective_protocol_filter(LoggingProtocolId protocol, std::string_view filter)
{
    switch (protocol)
    {
    case LoggingProtocolId::Ssm:
        if (filter.empty())
        {
            return fastecu::fail(ErrorKind::InvalidConfig, "SSM logging protocol filter is empty");
        }
        return std::string(filter);
    case LoggingProtocolId::MutDma:
        return "MUT_DMA";
    case LoggingProtocolId::Cdbg:
        return "CDBG";
    }
    return fastecu::fail(ErrorKind::InvalidConfig, "invalid logging protocol");
}

fastecu::Result<const LoggerParameter *> selected_parameter(const LoggerModel& model, LoggingProtocolId protocol,
                                                            std::string_view key, std::string_view id)
{
    const LoggerParameter *selected = nullptr;
    for (const auto& parameter : model.definition().parameters)
    {
        if (parameter.id != id || parameter.protocol != key ||
            (protocol == LoggingProtocolId::MutDma && !model.parameter_supported(key, id)))
        {
            continue;
        }
        if (selected != nullptr)
        {
            return fastecu::fail(
                ErrorKind::InvalidConfig,
                std::format("{} parameter {}: duplicate logging value id in selected protocol", key, id));
        }
        selected = &parameter;
    }
    return selected;
}
} // namespace

LoggingRunSnapshot::LoggingRunSnapshot(LoggingSession session, std::string protocol_key, LoggerSelection selection,
                                       std::vector<std::size_t> response_offsets,
                                       std::unordered_set<std::string> enabled_ids, LoggingTarget target)
    : session_(std::move(session)), protocol_key_(std::move(protocol_key)), selection_(std::move(selection)),
      response_offsets_(std::move(response_offsets)), enabled_ids_(std::move(enabled_ids)), target_(target)
{
}

const std::vector<LoggingMeasurement>& LoggingRunSnapshot::measurements() const
{
    return measurements_;
}
const LoggingMeasurement *LoggingRunSnapshot::find_measurement(LoggingMeasurementKind kind, std::string_view id) const
{
    for (const auto& item : measurements_)
    {
        if (item.kind == kind && item.identity.second == id)
        {
            return &item;
        }
    }
    return nullptr;
}
const LoggingSession& LoggingRunSnapshot::session() const
{
    return session_;
}
const std::string& LoggingRunSnapshot::protocol_key() const
{
    return protocol_key_;
}
const LoggerSelection& LoggingRunSnapshot::selection() const
{
    return selection_;
}
const std::vector<std::size_t>& LoggingRunSnapshot::response_offsets() const
{
    return response_offsets_;
}
LoggingTarget LoggingRunSnapshot::target() const
{
    return target_;
}
bool LoggingRunSnapshot::channel_enabled(std::string_view id) const
{
    return enabled_ids_.contains(std::string(id));
}

fastecu::Result<LoggingRunSnapshot> prepare_logging_run(const LoggerModel& model, LoggingProtocolId protocol,
                                                        std::string_view protocol_filter, LoggingPolicy policy,
                                                        LoggingTarget target)
{
    const auto filter = effective_protocol_filter(protocol, protocol_filter);
    if (!filter.has_value())
    {
        return std::unexpected(filter.error());
    }
    if (target != LoggingTarget::Ecu && target != LoggingTarget::Tcu)
    {
        return fastecu::fail(ErrorKind::InvalidConfig, std::format("{}: invalid logging target", *filter));
    }
    std::vector<LoggingChannel> channels;
    std::vector<std::size_t> response_offsets;
    std::unordered_set<std::string> participating_ids;
    std::unordered_set<std::string> enabled_ids;
    const auto& selected_ids = model.selection().lower_panel_ids;
    for (std::size_t slot = 0; slot < selected_ids.size(); ++slot)
    {
        const auto& id = selected_ids[slot];
        const auto selected = selected_parameter(model, protocol, *filter, id);
        if (!selected.has_value())
        {
            return std::unexpected(selected.error());
        }
        if (*selected == nullptr)
        {
            continue;
        }
        if (!participating_ids.insert(id).second)
        {
            return fastecu::fail(ErrorKind::InvalidConfig,
                                 std::format("{} parameter {}: duplicate lower-panel logging value id", *filter, id));
        }
        auto channel = prepare_logging_channel(**selected, protocol);
        if (!channel.has_value())
        {
            return std::unexpected(channel.error());
        }
        if (protocol != LoggingProtocolId::Ssm || model.parameter_supported(*filter, id))
        {
            enabled_ids.insert(id);
        }
        if (protocol == LoggingProtocolId::Ssm)
        {
            response_offsets.push_back(slot);
        }
        channels.push_back(std::move(*channel));
    }
    auto session = make_logging_session(protocol, std::move(channels), policy);
    if (!session.has_value())
    {
        return fastecu::fail(session.error().kind, std::format("{}: {}", *filter, session.error().detail));
    }
    LoggingRunSnapshot snapshot(std::move(*session), *filter, model.selection(), std::move(response_offsets),
                                std::move(enabled_ids), target);
    for (const auto& channel : snapshot.session().channels())
    {
        const auto *source = model.parameter(*filter, channel.id);
        snapshot.measurements_.push_back({.kind = LoggingMeasurementKind::Parameter,
                                          .identity = {*filter, channel.id},
                                          .channel_id = channel.id,
                                          .name = source->name,
                                          .unit = channel.unit,
                                          .decimal_precision = channel.decimal_precision,
                                          .support = model.parameter_support(*filter, channel.id)});
    }
    return snapshot;
}
} // namespace fastecu::logging
