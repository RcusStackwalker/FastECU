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
        if (p.protocol != protocol_arg.toStdString() || !loggerModel->parameter_supported(p.protocol, p.id))
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

void MainWindow::log_to_file()
{
    if (write_datalog_to_file)
    {
        if (!datalog_file_open)
        {
            QDateTime dateTime = dateTime.currentDateTime();
            QString dateTimeString = dateTime.toString("yyyy-MM-dd_hh'h'mm'm'ss's'");

            QString log_file_name = fastecu::ui::qs(configSession->effective_paths().datalog_files_directory);
            if (!log_file_name.endsWith('/'))
            {
                log_file_name.append("/");
            }
            log_file_name.append("fastecu_" + dateTimeString + ".csv");

            datalog_file.setFileName(log_file_name);
            if (!datalog_file.open(QIODevice::WriteOnly))
            {
                QMessageBox::information(this, tr("Unable to open file"), datalog_file.errorString());
                return;
            }
            else
            {
                datalog_file_open = true;
                log_file_timer->start();
            }

            datalog_file_outstream.setDevice(&datalog_file);
            datalog_file_outstream << "Time,";
            write_logger_csv_cells(true);
            datalog_file_outstream << "\n";
        }
        else
        {

            datalog_file_outstream << QString::number(log_file_timer->elapsed() / 1000.0F) << ",";
            write_logger_csv_cells(false);
            datalog_file_outstream << "\n";
        }
    }
}

void MainWindow::write_logger_csv_cells(bool header)
{
    const auto key = activeLoggingSnapshot ? activeLoggingSnapshot->protocol : protocol.toStdString();
    const auto& selection = loggerModel->selection();
    const auto parameters = [&](const auto& ids)
    {
        for (const auto& id : ids)
        {
            const auto *item = loggerModel->parameter(key, id);
            datalog_file_outstream << (item == nullptr ? QString{}
                                       : header        ? fastecu::ui::qs(item->name)
                                                       : loggerValues.parameter_value(key, id))
                                   << ",";
        }
    };
    parameters(selection.gauge_ids);
    parameters(selection.lower_panel_ids);
    for (const auto& id : selection.switch_ids)
    {
        const auto *item = loggerModel->switch_definition(key, id);
        datalog_file_outstream << (item == nullptr ? QString{}
                                   : header        ? fastecu::ui::qs(item->name)
                                                   : loggerValues.switch_value(key, id))
                               << ",";
    }
}
