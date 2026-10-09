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
            emit logD("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " + QString::number(index) +
                          " and bit " + QString::number(bit) + " is enabled: " + fastecu::ui::qs(p.id) + " " +
                          fastecu::ui::qs(p.name) + " 1",
                      true, true);
        }
    }
    loadLoggerSelection();

    updateLogboxes(protocolArg);
}

void MainWindow::logToFile()
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
            writeLoggerCsvRecord(true);
        }
        else
        {

            writeLoggerCsvRecord(false);
        }
    }
}

void MainWindow::writeLoggerCsvRecord(bool header)
{
    std::vector<std::string> fields{
        header ? "Time" : QString::number(static_cast<float>(log_file_timer_->elapsed()) / 1000.0F).toStdString()};
    const auto key = active_logging_snapshot_ ? active_logging_snapshot_->ProtocolKey() : protocol_.toStdString();
    const auto& selection = logger_model_->Selection();
    const auto parameters = [&](const auto& ids)
    {
        for (const auto& id : ids)
        {
            const auto *item = logger_model_->Parameter(key, id);
            fields.push_back(item == nullptr ? std::string{}
                             : header        ? item->name
                                             : logger_values_.ParameterValue(key, id).toStdString());
        }
    };
    parameters(selection.gauge_ids);
    parameters(selection.lower_panel_ids);
    for (const auto& id : selection.switch_ids)
    {
        const auto *item = logger_model_->SwitchDefinition(key, id);
        fields.push_back(item == nullptr ? std::string{}
                         : header        ? item->name
                                         : logger_values_.SwitchValue(key, id).toStdString());
    }
    const std::vector<std::string_view> fieldViews{fields.begin(), fields.end()};
    datalog_file_outstream_ << QString::fromStdString(fastecu::logging::SerializeLoggingCsvRecord(fieldViews));
}
