#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "src/algorithms/protocol/qt_compat/qt_bytes.h"

void MainWindow::parse_log_value_list(QByteArray received, const QString& protocol_arg)
{
    uint16_t enabled_log_value_count = 0;
    uint16_t enabled_log_switch_count = 0;
    uint16_t log_value_count = logValues->log_value_id.length();
    uint16_t log_switch_count = logValues->log_switch_id.length();

    received.remove(0, 5);

    // emit LOG_D(parse_message_to_hex(received), true, true);

    // emit LOG_D("Parsing log values list", true, true), true, true);
    for (int i = 0; i < log_value_count; i++)
    {
        if (logValues->log_value_protocol.at(i) == protocol_arg)
        {
            uint16_t ecu_byte_index = logValues->log_value_ecu_byte_index.at(i).toUInt();
            if (ecu_byte_index < received.length() && logValues->log_value_ecu_byte_index.at(i) != "No byte index")
            {
                // emit LOG_D("Value: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i), true,
                // true);
                uint8_t ecu_bit = logValues->log_value_ecu_bit.at(i).toUInt();
                uint16_t value = (uint8_t)received.at(ecu_byte_index);
                if (((value) & (1U << (ecu_bit))))
                {
                    logValues->log_value_enabled.replace(i, "1");
                    emit LOG_D("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " +
                                   QString::number(ecu_byte_index) + " and bit " + QString::number(ecu_bit) +
                                   " is enabled: " + logValues->log_value_id.at(i) + " " +
                                   logValues->log_value_name.at(i) + " " + logValues->log_value_enabled.at(i),
                               true, true);
                    enabled_log_value_count++;
                }
                else
                {
                    logValues->log_value_enabled.replace(i, "0");
                    // emit LOG_D("Disabled: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i) +
                    // " " + logValues->log_value_enabled.at(i), true, true);
                }
            }
            else
            {
                logValues->log_value_enabled[i] = "0";
            }
        }
    }
    // emit LOG_D("Log values list ready", true, true);

    // emit LOG_D("Parsing log switches list", true, true);
    for (int i = 0; i < log_switch_count; i++)
    {
        if (logValues->log_switch_protocol.at(i) == protocol_arg)
        {
            // 'switch_byte_index' is byte index in SSM init response
            uint16_t switch_byte_index = logValues->log_switch_ecu_byte_index.at(i).toUInt();
            if (switch_byte_index < received.length())
            {
                uint8_t switch_bit = logValues->log_switch_ecu_bit.at(i).toUInt();
                // emit LOG_D("1 " + switch_byte_index, true, true);
                uint8_t value = (uint8_t)received.at(switch_byte_index);
                // emit LOG_D("2", true, true);
                if (((value) & (1U << (switch_bit))))
                {
                    logValues->log_switch_enabled.replace(i, "1");
                    // emit LOG_D("Switch: " + logValues->log_switch_id.at(i) + " " + logValues->log_switch_name.at(i) +
                    // " " + logValues->log_switch_enabled.at(i), true, true);
                    enabled_log_switch_count++;
                }
                else
                {
                    logValues->log_switch_enabled.replace(i, "0");
                }
            }
        }
    }
    fileActions->read_logger_conf(logValues, ecuid, false);
    // emit LOG_D("Log switches list ready", true, true);

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

            QString log_file_name = configValues->datalog_files_directory;
            if (configValues->datalog_files_directory.at(configValues->datalog_files_directory.length() - 1) != '/')
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
            for (int j = 0; j < logValues->dashboard_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value_name.at(logValues->log_value_id.indexOf(
                                              logValues->dashboard_log_value_id.at(j), 0))
                                       << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value_name.at(logValues->log_value_id.indexOf(
                                              logValues->lower_panel_log_value_id.at(j), 0))
                                       << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_switch_name.at(logValues->log_switch_id.indexOf(
                                              logValues->lower_panel_switch_id.at(j), 0))
                                       << ",";
            }
            datalog_file_outstream << "\n";
        }
        else
        {

            datalog_file_outstream << QString::number(log_file_timer->elapsed() / 1000.0F) << ",";
            for (int j = 0; j < logValues->dashboard_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value.at(logValues->log_value_id.indexOf(
                                              logValues->dashboard_log_value_id.at(j), 0))
                                       << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value.at(logValues->log_value_id.indexOf(
                                              logValues->lower_panel_log_value_id.at(j), 0))
                                       << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_switch_state.at(logValues->log_switch_id.indexOf(
                                              logValues->lower_panel_switch_id.at(j), 0))
                                       << ",";
            }
            datalog_file_outstream << "\n";
        }
    }
}
