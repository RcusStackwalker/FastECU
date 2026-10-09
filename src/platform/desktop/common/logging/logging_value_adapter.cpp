#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <utility>

namespace fastecu::desktop::logging
{
void DesktopLoggerValues::initialize(const fastecu::logging::LoggerModel& model)
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
    const auto identity = snapshot.identities_by_id.find(sample.channel_id);
    if (identity == snapshot.identities_by_id.end())
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging sample id is not in the desktop snapshot");
    }
    const auto *channel = snapshot.session.FindChannel(sample.channel_id);
    if (channel == nullptr || identity->second.first != snapshot.protocol ||
        identity->second.second != sample.channel_id)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging snapshot identities and session disagree");
    }
    if (!snapshot.enabled_ids.contains(sample.channel_id))
    {
        return {};
    }
    if (!values.set_parameter_value(identity->second,
                                    format_logging_value(sample.numeric_value, channel->decimal_precision)))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging sample identity is not in the desktop values");
    }
    return {};
}
} // namespace fastecu::desktop::logging
