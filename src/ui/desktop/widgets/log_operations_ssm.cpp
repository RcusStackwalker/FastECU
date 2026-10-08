#include "src/ui/desktop/widgets/mainwindow.h"
#include "ui_mainwindow.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/ui/desktop/config_fields.h"

void MainWindow::parse_log_value_list(QByteArray received, const QString& protocol_arg)
{
    received.remove(0, 5);
    loggerModel->apply_capabilities(protocol_arg.toStdString(), bytes::view(received));
    for (const auto& p : loggerModel->definition().parameters)
    {
        if (p.protocol != protocol_arg.toStdString() || !loggerModel->parameter_available(p.protocol, p.id))
        {
            continue;
        }
        const auto index = static_cast<std::uint16_t>(QString::fromStdString(p.ecu_byte_index).toUInt());
        if (index < received.size())
        {
            const auto value = static_cast<std::uint8_t>(received.at(index));
            const auto bit = static_cast<std::uint8_t>(QString::fromStdString(p.ecu_bit).toUInt());
            emit LOG_D("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " + QString::number(index) +
                           " and bit " + QString::number(bit) + " is enabled: " + fastecu::ui::qs(p.id) + " " +
                           fastecu::ui::qs(p.name) + " 1",
                       true, true);
        }
    }
    load_logger_selection();

    update_logboxes(protocol_arg);
}

void MainWindow::report_logging_csv_error(const fastecu::Error& error)
{
    if (logging_csv_failed_)
    {
        return;
    }
    logging_csv_failed_ = true;
    const auto message = QString::fromStdString(error.detail);
    emit LOG_E(message, true, true);
    QMessageBox notice(this);
    notice.setWindowTitle(tr("Logging CSV"));
    notice.setTextFormat(Qt::PlainText);
    notice.setText(message);
    notice.setStandardButtons(QMessageBox::Ok);
    notice.exec();
}

void MainWindow::begin_logging_csv()
{
    if (!write_datalog_to_file || !activeLoggingSnapshot || logging_csv_failed_ || logging_csv_file_.is_open())
    {
        return;
    }
    const auto stem = "fastecu_" + QDateTime::currentDateTime().toString("yyyy-MM-dd_hh'h'mm'm'ss's'");
    const auto opened = logging_csv_file_.begin_run(
        *activeLoggingSnapshot, fastecu::ui::qs(configSession->effective_paths().datalog_files_directory), stem);
    if (!opened.has_value())
    {
        report_logging_csv_error(opened.error());
        return;
    }
    last_logging_csv_path_ = *opened;
}

void MainWindow::end_logging_csv()
{
    const auto closed = logging_csv_file_.end_run();
    if (!closed.has_value())
    {
        report_logging_csv_error(closed.error());
    }
}

void MainWindow::log_to_file()
{
    if (!write_datalog_to_file || !activeLoggingSnapshot || logging_csv_failed_)
    {
        return;
    }
    begin_logging_csv();
    if (!activeLoggingSnapshot || !logging_csv_file_.is_open())
    {
        return;
    }
    const auto elapsed = log_file_timer->isValid() ? log_file_timer->elapsed() : 0;
    const auto written = logging_csv_file_.append_row(loggerValues, std::chrono::milliseconds{elapsed});
    if (!written.has_value())
    {
        report_logging_csv_error(written.error());
    }
}
