#include "src/platform/desktop/common/logging/logging_value_adapter.h"

namespace fastecu::desktop::logging
{

QString format_logging_value(double value, int precision)
{
    return QString::number(value, 'f', precision);
}

fastecu::Status apply_log_sample(const LegacyLoggingMapping& mapping, const fastecu::logging::LogSample& sample,
                                 FileActions::LogValuesStructure& log_values)
{
    const auto legacy_index = mapping.index_by_id.find(sample.channel_id);
    if (legacy_index == mapping.index_by_id.end())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "logging sample id is not in the legacy mapping");
    }

    const int row = legacy_index->second;
    if (row < 0 || row >= log_values.log_value.size() || row >= log_values.log_value_id.size() ||
        row >= log_values.log_value_conversions.size() ||
        log_values.log_value_id.at(row).toStdString() != sample.channel_id)
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "legacy logging values no longer match the legacy mapping");
    }

    if (!mapping.enabled_ids.contains(sample.channel_id))
    {
        return {};
    }

    const auto& conversions = log_values.log_value_conversions.at(row);
    if (conversions.isEmpty())
    {
        return fastecu::fail(fastecu::ErrorKind::Internal, "legacy logging conversion is missing");
    }
    const QString format = QString::fromStdString(conversions.at(0).format);
    const QStringList format_fields = format.split('.');
    const int precision = format_fields.size() > 1 ? format_fields.at(1).count('0') : 0;
    log_values.log_value.replace(row, format_logging_value(sample.numeric_value, precision));
    return {};
}

} // namespace fastecu::desktop::logging
