#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <utility>

#include "src/backend/logging/logging_sample_resolution.h"

namespace fastecu::desktop::logging
{
void DesktopLoggerValues::initialize(const fastecu::logging::LoggerModel& model)
{
    parameters_.clear();
    switches_.clear();
    for (const auto& p : model.definition().parameters)
    {
        parameters_.emplace(fastecu::logging::LoggerIdentity{p.protocol, p.id}, QStringLiteral("0.00"));
    }
    for (const auto& p : model.definition().switches)
    {
        switches_.emplace(fastecu::logging::LoggerIdentity{p.protocol, p.id}, QStringLiteral("0"));
    }
}
QString DesktopLoggerValues::parameter_value(std::string_view protocol, std::string_view id) const
{
    const auto it = parameters_.find({std::string(protocol), std::string(id)});
    return it == parameters_.end() ? QString{} : it->second;
}
QString DesktopLoggerValues::switch_value(std::string_view protocol, std::string_view id) const
{
    const auto it = switches_.find({std::string(protocol), std::string(id)});
    return it == switches_.end() ? QString{} : it->second;
}
bool DesktopLoggerValues::set_parameter_value(const fastecu::logging::LoggerIdentity& identity, QString value)
{
    const auto it = parameters_.find(identity);
    if (it == parameters_.end())
    {
        return false;
    }
    it->second = std::move(value);
    return true;
}
QString format_logging_value(double value, int precision)
{
    return QString::number(value, 'f', precision);
}
fastecu::Status apply_log_sample(const DesktopLoggingSnapshot& snapshot, const fastecu::logging::LogSample& sample,
                                 DesktopLoggerValues& values)
{
    const auto resolved = fastecu::logging::resolve_log_sample(snapshot, sample);
    if (!resolved.has_value())
    {
        return std::unexpected(resolved.error());
    }
    if (!resolved->has_value())
    {
        return {};
    }
    const auto& value = **resolved;
    if (!values.set_parameter_value(value.identity, format_logging_value(value.numeric_value, value.decimal_precision)))
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "logging sample identity is not in the desktop values");
    }
    return {};
}
} // namespace fastecu::desktop::logging
