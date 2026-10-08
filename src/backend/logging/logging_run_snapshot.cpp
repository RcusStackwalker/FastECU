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

} // namespace

LoggingRunSnapshot::LoggingRunSnapshot(LoggingSession session, std::string protocol_key, LoggerSelection selection,
                                       std::vector<LoggingMeasurement> measurements, LoggingTarget target)
    : session_(std::move(session)), protocol_key_(std::move(protocol_key)), selection_(std::move(selection)),
      target_(target), measurements_(std::move(measurements))
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
LoggingTarget LoggingRunSnapshot::target() const
{
    return target_;
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
    std::vector<LoggingMeasurement> measurements;
    std::unordered_set<std::string> identities;
    const auto add = [&](LoggingMeasurementKind kind, const std::string& id) -> Status
    {
        const auto tag = std::format("{}:{}", kind == LoggingMeasurementKind::Parameter ? "parameter" : "switch", id);
        if (!identities.insert(tag).second)
        {
            return {};
        }
        const auto state = kind == LoggingMeasurementKind::Parameter ? model.parameter_support(*filter, id)
                                                                     : model.switch_support(*filter, id);
        if (state == EcuSupport::Unsupported)
        {
            return fail(
                ErrorKind::InvalidConfig,
                std::format("{} {}: selected measurement is known unsupported; remove or replace it", *filter, tag));
        }
        Result<LoggingChannel> prepared = fail(ErrorKind::InvalidConfig, "unresolved measurement");
        std::string name;
        std::size_t matches = 0;
        if (kind == LoggingMeasurementKind::Parameter)
        {
            for (const auto& source : model.definition().parameters)
            {
                if (source.protocol == *filter && source.id == id)
                {
                    ++matches;
                    prepared = prepare_logging_channel(source, protocol);
                    name = source.name;
                }
            }
        }
        else
        {
            for (const auto& source : model.definition().switches)
            {
                if (source.protocol == *filter && source.id == id)
                {
                    ++matches;
                    prepared = prepare_logging_switch(source, protocol);
                    name = source.name;
                }
            }
        }
        if (matches != 1)
        {
            return fail(ErrorKind::InvalidConfig,
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
    for (const auto& ids : {model.selection().gauge_ids, model.selection().lower_panel_ids})
    {
        for (const auto& id : ids)
        {
            const auto status = add(LoggingMeasurementKind::Parameter, id);
            if (!status.has_value())
            {
                return std::unexpected(status.error());
            }
        }
    }
    for (const auto& id : model.selection().switch_ids)
    {
        const auto status = add(LoggingMeasurementKind::Switch, id);
        if (!status.has_value())
        {
            return std::unexpected(status.error());
        }
    }
    auto dialect = mutdma::FreeformDialect::LegacyBe;
    if (protocol == LoggingProtocolId::MutDma)
    {
        std::size_t matches = 0;
        for (const auto& source : model.definition().protocols)
        {
            if (source.id != *filter)
            {
                continue;
            }
            if (++matches > 1)
            {
                return fail(ErrorKind::InvalidConfig, "MUT_DMA: duplicate protocol dialect declarations");
            }
            if (source.dialect == "oem-33520003")
            {
                dialect = mutdma::FreeformDialect::Oem33520003;
            }
            else if (!source.dialect.empty() && source.dialect != "legacy-be")
            {
                return fail(ErrorKind::InvalidConfig, std::format("MUT_DMA: unknown dialect '{}'", source.dialect));
            }
        }
    }
    auto session = make_logging_session(protocol, std::move(channels), policy, dialect);
    if (!session.has_value())
    {
        return fail(session.error().kind, std::format("{}: {}", *filter, session.error().detail));
    }
    return LoggingRunSnapshot(std::move(*session), *filter, model.selection(), std::move(measurements), target);
}
} // namespace fastecu::logging
