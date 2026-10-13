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

void MainWindow::reportLoggingCsvError(const QString& message)
{
    emit logE(message, true, true);
    QMessageBox notice(this);
    notice.setWindowTitle(tr("Logging CSV"));
    notice.setTextFormat(Qt::PlainText);
    notice.setText(message);
    notice.setStandardButtons(QMessageBox::Ok);
    notice.exec();
}
