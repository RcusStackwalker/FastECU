#include "src/ui/desktop/widgets/mainwindow.h"

#include <string_view>
#include <vector>

#include "src/backend/logging/logging_csv_record.h"
#include "ui_mainwindow.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/ui/desktop/config_fields.h"

void MainWindow::parseLogValueList(QByteArray received, const QString& protocolArg)
{
    received.remove(0, 5);
    received.chop(1); // Identification framing is validated by the backend; checksum is not capability data.
    logger_model_->ApplyCapabilities(protocolArg.toStdString(), bytes::View(received));
    for (const auto& p : logger_model_->Definition().parameters)
    {
        if (p.protocol != protocolArg.toStdString() ||
            logger_model_->ParameterSupport(p.protocol, p.id) != fastecu::logging::EcuSupport::kSupported)
        {
            continue;
        }
        const auto index = static_cast<std::uint16_t>(QString::fromStdString(p.ecu_byte_index).toUInt());
        if (index < received.size())
        {
            const auto value = static_cast<std::uint8_t>(received.at(index));
            const auto bit = static_cast<std::uint8_t>(QString::fromStdString(p.ecu_bit).toUInt());
            emit logD("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " + QString::number(index) +
                          " and bit " + QString::number(bit) + " is enabled: " + fastecu::ui::qs(p.id) + " " +
                          fastecu::ui::qs(p.name) + " 1",
                      true, true);
        }
    }
    loadLoggerSelection();

    updateLogboxes(protocolArg);
}

void MainWindow::reportLoggingCsvError(const fastecu::Error& error)
{
    if (logging_csv_failed_)
    {
        return;
    }
    logging_csv_failed_ = true;
    const auto message = QString::fromStdString(error.detail);
    emit logE(message, true, true);
    QMessageBox notice(this);
    notice.setWindowTitle(tr("Logging CSV"));
    notice.setTextFormat(Qt::PlainText);
    notice.setText(message);
    notice.setStandardButtons(QMessageBox::Ok);
    notice.exec();
}

void MainWindow::beginLoggingCsv()
{
    if (!write_datalog_to_file_ || !active_logging_snapshot_ || logging_csv_failed_ || logging_csv_file_.IsOpen())
    {
        return;
    }
    const auto stem = "fastecu_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'");
    const auto opened = logging_csv_file_.BeginRun(
        *active_logging_snapshot_, fastecu::ui::qs(config_session_->EffectivePaths().datalog_files_directory), stem);
    if (!opened.has_value())
    {
        reportLoggingCsvError(opened.error());
        return;
    }
    last_logging_csv_path_ = *opened;
}

void MainWindow::endLoggingCsv()
{
    const auto closed = logging_csv_file_.EndRun();
    if (!closed.has_value())
    {
        reportLoggingCsvError(closed.error());
    }
}

void MainWindow::logToFile()
{
    if (!write_datalog_to_file_ || !active_logging_snapshot_ || logging_csv_failed_)
    {
        return;
    }
    beginLoggingCsv();
    if (!active_logging_snapshot_ || !logging_csv_file_.IsOpen())
    {
        return;
    }
    const auto elapsed = log_file_timer_->isValid() ? log_file_timer_->elapsed() : 0;
    const auto written = logging_csv_file_.AppendRow(logger_values_, std::chrono::milliseconds{elapsed});
    if (!written.has_value())
    {
        reportLoggingCsvError(written.error());
    }
}
