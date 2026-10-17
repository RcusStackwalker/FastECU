#include "src/backend/logging/logging_run_snapshot.h"

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

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

struct Resolved
{
    Result<LoggingChannel> channel = Fail(ErrorKind::kInvalidConfig, "unresolved measurement");
    std::string name;
    std::size_t matches = 0;
};

// The last definition matching the filter and id; callers reject any count but one.
template <typename Source>
Resolved Resolve(const std::vector<Source>& sources, std::string_view filter, std::string_view id,
                 LoggingProtocolId protocol, Result<LoggingChannel> (*prepare)(const Source&, LoggingProtocolId))
{
    Resolved resolved;
    for (const auto& source : sources)
    {
        if (source.protocol == filter && source.id == id)
        {
            ++resolved.matches;
            resolved.channel = prepare(source, protocol);
            resolved.name = source.name;
        }
    }
    return resolved;
}

// Prepares each selected measurement once, as a channel named by its kind and id.
class MeasurementCollector
{
  public:
    MeasurementCollector(const LoggerModel& model, LoggingProtocolId protocol, std::string_view filter)
        : model_(model), protocol_(protocol), filter_(filter)
    {
    }

    Status AddAll(LoggingMeasurementKind kind, const std::vector<std::string>& ids)
    {
        for (const auto& id : ids)
        {
            if (auto status = Add(kind, id); !status.has_value())
            {
                return status;
            }
        }
        return {};
    }

    std::vector<LoggingChannel> channels;
    std::vector<LoggingMeasurement> measurements;

  private:
    Status Add(LoggingMeasurementKind kind, const std::string& id)
    {
        const bool parameter = kind == LoggingMeasurementKind::kParameter;
        const auto tag = std::format("{}:{}", parameter ? "parameter" : "switch", id);
        if (!identities_.insert(tag).second)
        {
            return {};
        }
        const auto state = parameter ? model_.ParameterSupport(filter_, id) : model_.SwitchSupport(filter_, id);
        if (state == EcuSupport::kUnsupported)
        {
            return Fail(
                ErrorKind::kInvalidConfig,
                std::format("{} {}: selected measurement is known unsupported; remove or replace it", filter_, tag));
        }
        Resolved resolved =
            parameter ? Resolve(model_.Definition().parameters, filter_, id, protocol_, &PrepareLoggingChannel)
                      : Resolve(model_.Definition().switches, filter_, id, protocol_, &PrepareLoggingSwitch);
        if (resolved.matches != 1)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("{} {}: {} definition; remove or replace the selected entry", filter_, tag,
                                    resolved.matches == 0 ? "unresolved" : "duplicate"));
        }
        if (!resolved.channel.has_value())
        {
            return std::unexpected(resolved.channel.error());
        }
        resolved.channel->id = tag;
        measurements.push_back({.kind = kind,
                                .identity = {std::string(filter_), id},
                                .channel_id = tag,
                                .name = std::move(resolved.name),
                                .unit = resolved.channel->unit,
                                .decimal_precision = resolved.channel->decimal_precision,
                                .support = state});
        channels.push_back(std::move(*resolved.channel));
        return {};
    }

    const LoggerModel& model_;
    LoggingProtocolId protocol_;
    std::string_view filter_;
    std::unordered_set<std::string> identities_;
};
} // namespace

LoggingRunSnapshot::LoggingRunSnapshot(LoggingSession session, std::string protocol_key, LoggerSelection selection,
                                       std::vector<LoggingMeasurement> measurements, LoggingTarget target)
    : session_(std::move(session)), protocol_key_(std::move(protocol_key)), selection_(std::move(selection)),
      target_(target), measurements_(std::move(measurements))
{
}

const std::vector<LoggingMeasurement>& LoggingRunSnapshot::Measurements() const
{
    return measurements_;
}
const LoggingMeasurement *LoggingRunSnapshot::FindMeasurement(LoggingMeasurementKind kind, std::string_view id) const
{
    const auto item = std::ranges::find_if(measurements_, [&](const LoggingMeasurement& measurement)
                                           { return measurement.kind == kind && measurement.identity.second == id; });
    return item == measurements_.end() ? nullptr : std::to_address(item);
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
LoggingTarget LoggingRunSnapshot::Target() const
{
    return target_;
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
    MeasurementCollector collector(model, protocol, *filter);
    const LoggerSelection& selection = model.Selection();
    for (const auto& [kind, ids] : {std::pair{LoggingMeasurementKind::kParameter, &selection.gauge_ids},
                                    std::pair{LoggingMeasurementKind::kParameter, &selection.lower_panel_ids},
                                    std::pair{LoggingMeasurementKind::kSwitch, &selection.switch_ids}})
    {
        if (const auto status = collector.AddAll(kind, *ids); !status.has_value())
        {
            return std::unexpected(status.error());
        }
    }
    auto session = MakeLoggingSession(protocol, std::move(collector.channels), policy);
    if (!session.has_value())
    {
        return Fail(session.error().kind, std::format("{}: {}", *filter, session.error().detail));
    }
    return LoggingRunSnapshot(std::move(*session), *filter, selection, std::move(collector.measurements), target);
}
} // namespace fastecu::logging
