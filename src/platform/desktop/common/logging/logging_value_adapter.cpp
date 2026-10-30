#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <utility>

#include "src/backend/logging/logging_sample_resolution.h"

namespace fastecu::desktop::logging
{
void DesktopLoggerValues::Initialize(const fastecu::logging::LoggerModel& model)
{
    parameters_.clear();
    switches_.clear();
    for (const auto& p : model.Definition().parameters)
    {
        parameters_.emplace(fastecu::logging::LoggerIdentity{p.protocol, p.id}, QStringLiteral("0.00"));
    }
    for (const auto& p : model.Definition().switches)
    {
        switches_.emplace(fastecu::logging::LoggerIdentity{p.protocol, p.id}, QStringLiteral("0"));
    }
}
void DesktopLoggerValues::BeginRun(const fastecu::logging::LoggingRunSnapshot& snapshot)
{
    for (const auto& item : snapshot.Measurements())
    {
        auto& cache = item.kind == fastecu::logging::LoggingMeasurementKind::kParameter ? parameters_ : switches_;
        cache.insert_or_assign(item.identity, QString{});
    }
}
bool DesktopLoggerValues::SetSwitchValue(const fastecu::logging::LoggerIdentity& identity, QString value)
{
    const auto it = switches_.find(identity);
    if (it == switches_.end())
    {
        return false;
    }
    it->second = std::move(value);
    return true;
}
QString DesktopLoggerValues::ParameterValue(std::string_view protocol, std::string_view id) const
{
    const auto it = parameters_.find({std::string(protocol), std::string(id)});
    return it == parameters_.end() ? QString{} : it->second;
}
QString DesktopLoggerValues::SwitchValue(std::string_view protocol, std::string_view id) const
{
    const auto it = switches_.find({std::string(protocol), std::string(id)});
    return it == switches_.end() ? QString{} : it->second;
}
bool DesktopLoggerValues::SetParameterValue(const fastecu::logging::LoggerIdentity& identity, QString value)
{
    const auto it = parameters_.find(identity);
    if (it == parameters_.end())
    {
        return false;
    }
    it->second = std::move(value);
    return true;
}
QString FormatLoggingValue(double value, int precision)
{
    return QString::number(value, 'f', precision);
}
fastecu::Status ApplyLogSample(const fastecu::logging::LoggingRunSnapshot& snapshot,
                               const fastecu::logging::LogSample& sample, DesktopLoggerValues& values)
{
    const auto resolved = fastecu::logging::ResolveLogSample(snapshot, sample);
    if (!resolved.has_value())
    {
        return std::unexpected(resolved.error());
    }
    const auto& value = *resolved;
    const auto formatted =
        value.numeric_value.has_value() ? FormatLoggingValue(*value.numeric_value, value.decimal_precision) : QString{};
    const bool updated = value.kind == fastecu::logging::LoggingMeasurementKind::kParameter
                             ? values.SetParameterValue(value.identity, formatted)
                             : values.SetSwitchValue(value.identity, formatted);
    if (!updated)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging sample identity is not in the desktop values");
    }
    return {};
}
} // namespace fastecu::desktop::logging
