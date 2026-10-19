#include "src/platform/desktop/common/logging/logging_csv_file.h"

#include <algorithm>
#include <iterator>
#include <ranges>
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
bool LoggingCsvFile::IsOpen() const
{
    return file_ != nullptr && file_->isOpen();
}
Status LoggingCsvFile::WriteRecord(const std::vector<std::string>& fields)
{
    auto views = fields | std::views::transform([](const auto& field) { return std::string_view{field}; }) |
                 std::ranges::to<std::vector<std::string_view>>();
    const auto record = fastecu::logging::SerializeLoggingCsvRecord(views);
    const auto size = static_cast<qint64>(record.size());
    if (file_->write(record.data(), size) != size || !file_->flush())
    {
        const auto error =
            QStringLiteral("Unable to write logging CSV %1: %2").arg(file_->fileName(), file_->errorString());
        file_->close();
        return Fail(ErrorKind::kInternal, error.toStdString());
    }
    return {};
}
Result<QString> LoggingCsvFile::BeginRun(const fastecu::logging::LoggingRunSnapshot& snapshot, const QString& directory,
                                         const QString& name_stem)
{
    if (file_ == nullptr || IsOpen())
    {
        return Fail(ErrorKind::kInvalidConfig, "CSV storage missing or previous run still owns it");
    }
    auto columns = fastecu::logging::PrepareLoggingCsvColumns(snapshot);
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
            return Fail(
                ErrorKind::kInvalidConfig,
                QStringLiteral("Unable to create logging CSV %1: %2").arg(path, file_->errorString()).toStdString());
        }
    }
    std::vector<std::string> header{"Time"};
    std::ranges::transform(columns_, std::back_inserter(header), [](const auto& column) { return column.name; });
    const auto written = WriteRecord(header);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return file_->fileName();
}
Status LoggingCsvFile::AppendRow(const DesktopLoggerValues& values, std::chrono::milliseconds elapsed)
{
    if (!IsOpen())
    {
        return Fail(ErrorKind::kInternal, "no CSV file owned by this run");
    }
    std::vector<std::string> fields{QString::number(static_cast<float>(elapsed.count()) / 1000.0F).toStdString()};
    for (const auto& column : columns_)
    {
        const auto& [protocol, id] = column.identity;
        const auto value = column.kind == fastecu::logging::LoggingMeasurementKind::kParameter
                               ? values.ParameterValue(protocol, id)
                               : values.SwitchValue(protocol, id);
        fields.push_back(value.toStdString());
    }
    return WriteRecord(fields);
}
Status LoggingCsvFile::EndRun()
{
    columns_.clear();
    if (!IsOpen())
    {
        return {};
    }
    const bool flushed = file_->flush();
    const auto error = file_->errorString();
    file_->close();
    if (!flushed)
    {
        return Fail(ErrorKind::kInternal, QStringLiteral("Unable to flush logging CSV: %1").arg(error).toStdString());
    }
    return {};
}
} // namespace fastecu::desktop::logging
