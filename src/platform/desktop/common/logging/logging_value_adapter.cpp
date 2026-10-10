#include "src/platform/desktop/common/logging/logging_value_adapter.h"

#include <utility>

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
fastecu::Status ApplyLogSample(const DesktopLoggingSnapshot& snapshot, const fastecu::logging::LogSample& sample,
                               DesktopLoggerValues& values)
{
    const auto *channel = snapshot.Session().FindChannel(sample.channel_id);
    if (channel == nullptr)
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging sample id is not in the run snapshot");
    }
    if (!snapshot.ChannelEnabled(sample.channel_id))
    {
        return {};
    }
    const fastecu::logging::LoggerIdentity identity{snapshot.ProtocolKey(), channel->id};
    if (!values.SetParameterValue(identity, FormatLoggingValue(sample.numeric_value, channel->decimal_precision)))
    {
        return fastecu::Fail(fastecu::ErrorKind::kInternal, "logging sample identity is not in the desktop values");
    }
    return {};
}
} // namespace fastecu::desktop::logging
