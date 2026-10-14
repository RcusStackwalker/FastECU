#include "src/backend/logging/logging_run_snapshot.h"

#include <algorithm>
#include <format>
#include <memory>
#include <utility>
#include <unordered_set>

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
    std::vector<LoggingChannel> channels;
    std::vector<LoggingMeasurement> measurements;
    std::unordered_set<std::string> identities;
    const auto add = [&](LoggingMeasurementKind kind, const std::string& id) -> Status
    {
        const auto tag = std::format("{}:{}", kind == LoggingMeasurementKind::kParameter ? "parameter" : "switch", id);
        if (!identities.insert(tag).second)
        {
            return {};
        }
        const auto state = kind == LoggingMeasurementKind::kParameter ? model.ParameterSupport(*filter, id)
                                                                      : model.SwitchSupport(*filter, id);
        if (state == EcuSupport::kUnsupported)
        {
            return Fail(
                ErrorKind::kInvalidConfig,
                std::format("{} {}: selected measurement is known unsupported; remove or replace it", *filter, tag));
        }
        Result<LoggingChannel> prepared = Fail(ErrorKind::kInvalidConfig, "unresolved measurement");
        std::string name;
        std::size_t matches = 0;
        if (kind == LoggingMeasurementKind::kParameter)
        {
            for (const auto& source : model.Definition().parameters)
            {
                if (source.protocol == *filter && source.id == id)
                {
                    ++matches;
                    prepared = PrepareLoggingChannel(source, protocol);
                    name = source.name;
                }
            }
        }
        else
        {
            for (const auto& source : model.Definition().switches)
            {
                if (source.protocol == *filter && source.id == id)
                {
                    ++matches;
                    prepared = PrepareLoggingSwitch(source, protocol);
                    name = source.name;
                }
            }
        }
        if (matches != 1)
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("{} {}: {} definition; remove or replace the selected entry", *filter, tag,
                                    matches == 0 ? "unresolved" : "duplicate"));
        }
        if (!prepared.has_value())
        {
            return std::unexpected(prepared.error());
        }
        prepared->id = tag;
        measurements.push_back({.kind = kind,
                                .identity = {*filter, id},
                                .channel_id = tag,
                                .name = std::move(name),
                                .unit = prepared->unit,
                                .decimal_precision = prepared->decimal_precision,
                                .support = state});
        channels.push_back(std::move(*prepared));
        return {};
    };
    for (const auto& ids : {model.Selection().gauge_ids, model.Selection().lower_panel_ids})
    {
        for (const auto& id : ids)
        {
            const auto status = add(LoggingMeasurementKind::kParameter, id);
            if (!status.has_value())
            {
                return std::unexpected(status.error());
            }
        }
    }
    for (const auto& id : model.Selection().switch_ids)
    {
        const auto status = add(LoggingMeasurementKind::kSwitch, id);
        if (!status.has_value())
        {
            return std::unexpected(status.error());
        }
    }
    auto session = MakeLoggingSession(protocol, std::move(channels), policy);
    if (!session.has_value())
    {
        return Fail(session.error().kind, std::format("{}: {}", *filter, session.error().detail));
    }
    return LoggingRunSnapshot(std::move(*session), *filter, model.Selection(), std::move(measurements), target);
}
} // namespace fastecu::logging
