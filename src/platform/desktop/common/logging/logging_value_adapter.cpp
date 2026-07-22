#include "src/platform/desktop/common/logging/logging_value_adapter.h"

namespace fastecu::desktop::logging
{

QString format_logging_value(double value, int precision)
{
    return QString::number(value, 'f', precision);
}

fastecu::Status apply_log_sample(const DesktopLoggingSnapshot& snapshot,
                                 const fastecu::logging::LogSample& sample,
                                 FileActions::LogValuesStructure& log_values)
{
    const auto legacy_index = snapshot.index_by_id.find(sample.channel_id);
    if (legacy_index == snapshot.index_by_id.end())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal,
                             "logging sample id is not in the desktop snapshot");
    }

    const auto *channel = snapshot.session.find_channel(sample.channel_id);
    if (channel == nullptr)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal,
                             "logging snapshot map and session disagree");
    }

    if (!snapshot.enabled_ids.contains(sample.channel_id))
    {
        return {};
    }

    const int row = legacy_index->second;
    if (row < 0 || row >= log_values.log_value.size() || row >= log_values.log_value_id.size() ||
        log_values.log_value_id.at(row).toStdString() != sample.channel_id)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal,
                             "legacy logging values no longer match the desktop snapshot");
    }

    log_values.log_value.replace(row,
                                 format_logging_value(sample.numeric_value,
                                                      channel->decimal_precision));
    return {};
}

} // namespace fastecu::desktop::logging
