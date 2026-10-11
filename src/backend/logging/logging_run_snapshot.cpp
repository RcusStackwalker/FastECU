#include "src/backend/logging/logging_run_snapshot.h"

#include <format>
#include <utility>

#include "src/backend/logging/logging_channel_preparation.h"

namespace fastecu::logging
{
namespace
{
fastecu::Result<std::string> EffectiveProtocolFilter(LoggingProtocolId protocol, std::string_view filter)
{
    switch (protocol)
    {
    case LoggingProtocolId::kSsm:
        if (filter.empty())
        {
            return fastecu::Fail(ErrorKind::kInvalidConfig, "SSM logging protocol filter is empty");
        }
        return std::string(filter);
    case LoggingProtocolId::kMutDma:
        return "MUT_DMA";
    case LoggingProtocolId::kCdbg:
        return "CDBG";
    }
    return fastecu::Fail(ErrorKind::kInvalidConfig, "invalid logging protocol");
}

fastecu::Result<const LoggerParameter *> FindSelectedParameter(const LoggerModel& model, LoggingProtocolId protocol,
                                                               std::string_view key, std::string_view id)
{
    const LoggerParameter *selected = nullptr;
    for (const auto& parameter : model.Definition().parameters)
    {
        if (parameter.id != id || parameter.protocol != key ||
            (protocol == LoggingProtocolId::kMutDma && !model.ParameterSupported(key, id)))
        {
            continue;
        }
        if (selected != nullptr)
        {
            return fastecu::Fail(
                ErrorKind::kInvalidConfig,
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

const LoggingSession& LoggingRunSnapshot::Session() const
{
    return session_;
}
const std::string& LoggingRunSnapshot::ProtocolKey() const
{
    return protocol_key_;
}
const LoggerSelection& LoggingRunSnapshot::Selection() const
{
    return selection_;
}
const std::vector<std::size_t>& LoggingRunSnapshot::ResponseOffsets() const
{
    return response_offsets_;
}
LoggingTarget LoggingRunSnapshot::Target() const
{
    return target_;
}
bool LoggingRunSnapshot::ChannelEnabled(std::string_view id) const
{
    return enabled_ids_.contains(std::string(id));
}

fastecu::Result<LoggingRunSnapshot> PrepareLoggingRun(const LoggerModel& model, LoggingProtocolId protocol,
                                                      std::string_view protocol_filter, LoggingPolicy policy,
                                                      LoggingTarget target)
{
    const auto filter = EffectiveProtocolFilter(protocol, protocol_filter);
    if (!filter.has_value())
    {
        return std::unexpected(filter.error());
    }
    if (target != LoggingTarget::kEcu && target != LoggingTarget::kTcu)
    {
        return fastecu::Fail(ErrorKind::kInvalidConfig, std::format("{}: invalid logging target", *filter));
    }
    std::vector<LoggingChannel> channels;
    std::vector<std::size_t> response_offsets;
    std::unordered_set<std::string> participating_ids;
    std::unordered_set<std::string> enabled_ids;
    const auto& selected_ids = model.Selection().lower_panel_ids;
    for (std::size_t slot = 0; slot < selected_ids.size(); ++slot)
    {
        const auto& id = selected_ids[slot];
        const auto selected = FindSelectedParameter(model, protocol, *filter, id);
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
            return fastecu::Fail(ErrorKind::kInvalidConfig,
                                 std::format("{} parameter {}: duplicate lower-panel logging value id", *filter, id));
        }
        auto channel = PrepareLoggingChannel(**selected, protocol);
        if (!channel.has_value())
        {
            return std::unexpected(channel.error());
        }
        if (protocol != LoggingProtocolId::kSsm || model.ParameterSupported(*filter, id))
        {
            enabled_ids.insert(id);
        }
        if (protocol == LoggingProtocolId::kSsm)
        {
            response_offsets.push_back(slot);
        }
        channels.push_back(std::move(*channel));
    }
    auto session = MakeLoggingSession(protocol, std::move(channels), policy);
    if (!session.has_value())
    {
        return fastecu::Fail(session.error().kind, std::format("{}: {}", *filter, session.error().detail));
    }
    return LoggingRunSnapshot(std::move(*session), *filter, model.Selection(), std::move(response_offsets),
                              std::move(enabled_ids), target);
}
} // namespace fastecu::logging
