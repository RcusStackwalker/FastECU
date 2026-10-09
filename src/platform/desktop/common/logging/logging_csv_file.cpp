#include "src/platform/desktop/common/logging/logging_csv_file.h"

#include <string_view>
#include <utility>
#include <QDir>
#include <QFileInfo>

#include "src/backend/logging/logging_csv_record.h"

namespace fastecu::desktop::logging
{
LoggingCsvFile::LoggingCsvFile(std::unique_ptr<QFile> file) : file_(std::move(file))
{
}
bool LoggingCsvFile::is_open() const
{
    return file_ != nullptr && file_->isOpen();
}
Status LoggingCsvFile::write_record(const std::vector<std::string>& fields)
{
    std::vector<std::string_view> views;
    views.reserve(fields.size());
    for (const auto& field : fields)
    {
        views.push_back(field);
    }
    const auto record = fastecu::logging::serialize_logging_csv_record(views);
    const auto size = static_cast<qint64>(record.size());
    if (file_->write(record.data(), size) != size || !file_->flush())
    {
        const auto error =
            QStringLiteral("Unable to write logging CSV %1: %2").arg(file_->fileName(), file_->errorString());
        file_->close();
        return fail(ErrorKind::Internal, error.toStdString());
    }
    return {};
}
Result<QString> LoggingCsvFile::begin_run(const fastecu::logging::LoggingRunSnapshot& snapshot,
                                          const QString& directory, const QString& name_stem)
{
    if (file_ == nullptr || is_open())
    {
        return fail(ErrorKind::InvalidConfig, "CSV storage missing or previous run still owns it");
    }
    auto columns = fastecu::logging::prepare_logging_csv_columns(snapshot);
    if (!columns.has_value())
    {
        return std::unexpected(columns.error());
    }
    columns_ = std::move(*columns);
    for (qulonglong suffix = 0;; ++suffix)
    {
        const auto name = name_stem + (suffix == 0 ? QString{} : "_" + QString::number(suffix)) + ".csv";
        const auto path = QDir(directory).filePath(name);
        file_->setFileName(path);
        if (file_->open(QIODevice::NewOnly | QIODevice::WriteOnly))
        {
            break;
        }
        if (!QFileInfo::exists(path))
        {
            return fail(
                ErrorKind::InvalidConfig,
                QStringLiteral("Unable to create logging CSV %1: %2").arg(path, file_->errorString()).toStdString());
        }
    }
    std::vector<std::string> header{"Time"};
    for (const auto& column : columns_)
    {
        header.push_back(column.name);
    }
    const auto written = write_record(header);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return file_->fileName();
}
Status LoggingCsvFile::append_row(const DesktopLoggerValues& values, std::chrono::milliseconds elapsed)
{
    if (!is_open())
    {
        return fail(ErrorKind::Internal, "no CSV file owned by this run");
    }
    std::vector<std::string> fields{QString::number(static_cast<float>(elapsed.count()) / 1000.0F).toStdString()};
    for (const auto& column : columns_)
    {
        const auto& [protocol, id] = column.identity;
        const auto value = column.kind == fastecu::logging::LoggingMeasurementKind::Parameter
                               ? values.parameter_value(protocol, id)
                               : values.switch_value(protocol, id);
        fields.push_back(value.toStdString());
    }
    return write_record(fields);
}
Status LoggingCsvFile::end_run()
{
    columns_.clear();
    if (!is_open())
    {
        return {};
    }
    const bool flushed = file_->flush();
    const auto error = file_->errorString();
    file_->close();
    if (!flushed)
    {
        return fail(ErrorKind::Internal, QStringLiteral("Unable to flush logging CSV: %1").arg(error).toStdString());
    }
    return {};
}
} // namespace fastecu::desktop::logging
