#include "src/ui/desktop/widgets/mainwindow.h"
#include "ui_mainwindow.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"
#include "src/ui/desktop/config_fields.h"

void MainWindow::parse_log_value_list(QByteArray received, const QString& protocolArg)
{
    received.remove(0, 5);
    logger_model_->ApplyCapabilities(protocolArg.toStdString(), bytes::View(received));
    for (const auto& p : logger_model_->Definition().parameters)
    {
        if (p.protocol != protocolArg.toStdString() || !logger_model_->ParameterSupported(p.protocol, p.id))
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

    update_logboxes(protocolArg);
}

void MainWindow::log_to_file()
{
    if (write_datalog_to_file_)
    {
        if (!datalog_file_open_)
        {
            QDateTime dateTime = dateTime.currentDateTime();
            QString dateTimeString = dateTime.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

            QString logFileName = fastecu::ui::qs(config_session_->EffectivePaths().datalog_files_directory);
            if (!logFileName.endsWith('/'))
            {
                logFileName.append("/");
            }
            logFileName.append("fastecu_" + dateTimeString + ".csv");

            datalog_file_.setFileName(logFileName);
            if (!datalog_file_.open(QIODevice::WriteOnly))
            {
                QMessageBox::information(this, tr("Unable to open file"), datalog_file_.errorString());
                return;
            }
            else
            {
                datalog_file_open_ = true;
                log_file_timer_->start();
            }

            datalog_file_outstream_.setDevice(&datalog_file_);
            datalog_file_outstream_ << "Time,";
            write_logger_csv_cells(true);
            datalog_file_outstream_ << "\n";
        }
        else
        {

            datalog_file_outstream_ << QString::number(static_cast<float>(log_file_timer_->elapsed()) / 1000.0F) << ",";
            write_logger_csv_cells(false);
            datalog_file_outstream_ << "\n";
        }
    }
}

void MainWindow::write_logger_csv_cells(bool header)
{
    const auto key = active_logging_snapshot_ ? active_logging_snapshot_->protocol : protocol_.toStdString();
    const auto& selection = logger_model_->Selection();
    const auto parameters = [&](const auto& ids)
    {
        for (const auto& id : ids)
        {
            const auto *item = logger_model_->Parameter(key, id);
            datalog_file_outstream_ << (item == nullptr ? QString{}
                                        : header        ? fastecu::ui::qs(item->name)
                                                        : logger_values_.ParameterValue(key, id))
                                    << ",";
        }
    };
    parameters(selection.gauge_ids);
    parameters(selection.lower_panel_ids);
    for (const auto& id : selection.switch_ids)
    {
        const auto *item = logger_model_->SwitchDefinition(key, id);
        datalog_file_outstream_ << (item == nullptr ? QString{}
                                    : header        ? fastecu::ui::qs(item->name)
                                                    : logger_values_.SwitchValue(key, id))
                                << ",";
    }
}
