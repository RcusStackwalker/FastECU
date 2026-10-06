#include "src/ui/desktop/widgets/dataterminal.h"
#include "src/ui/desktop/diagnostic_link_io.h"

#include "src/backend/diagnostics/terminal_script.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

#include <QFile>
#include <QtGlobal>

#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

DataTerminal::DataTerminal(fastecu::diagnostics::IDiagnosticLink& link_arg, QWidget *parent)
    : QDialog(parent), ui{std::make_unique<Ui::DataTerminalWindow>()}
{
    ui->setupUi(this);

    this->link = &link_arg;

    // Set initial values
    ui->klineProtocol->addItem("SSM");
    ui->klineProtocol->addItem("iso14230");

    ui->klineBaudRate->setText("4800");

    ui->klineDataBits->addItem("7");
    ui->klineDataBits->addItem("8");
    ui->klineDataBits->addItem("9");
    ui->klineDataBits->setCurrentIndex(1);

    ui->klineStopBits->addItem("1");
    ui->klineStopBits->addItem("2");

    ui->klineParity->addItem("None");
    ui->klineParity->addItem("Odd");
    ui->klineParity->addItem("Even");

    ui->klineTesterId->setText("F0");
    ui->klineTargetId->setText("10");

    ui->canProtocol->addItem("CAN");
    ui->canProtocol->addItem("iso15765");
    ui->canProtocol->setCurrentIndex(1);

    ui->canBaudRate->setText("500000");

    ui->canIdLength->addItem("11bit");
    ui->canIdLength->addItem("29bit");

    ui->canTesterId->setText("7E0");
    ui->canTargetId->setText("7E8");

    connect(ui->klineProtocol, SIGNAL(currentIndexChanged(int)), this, SLOT(protocolTypeChanged(int)));
    connect(ui->klineListen, SIGNAL(clicked(bool)), this, SLOT(listenInterface()));
    connect(ui->sendKlineMessage, SIGNAL(clicked(bool)), this, SLOT(sendToInterface()));

    connect(ui->canProtocol, SIGNAL(currentIndexChanged(int)), this, SLOT(protocolTypeChanged(int)));
    connect(ui->canListen, SIGNAL(clicked(bool)), this, SLOT(listenInterface()));
    connect(ui->sendCanMessage, SIGNAL(clicked(bool)), this, SLOT(sendToInterface()));

    this->show();
}

DataTerminal::~DataTerminal()
{
}

void DataTerminal::protocolTypeChanged(int)
{
    emit LOG_D("Change protocol type", true, true);
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();

    QComboBox *protocolType = (QComboBox *)obj;

    if (protocolType)
    {
        if (interfaceTypeName == "klineProtocol")
        {
            emit LOG_D("K-Line protocol type changed to: " + protocolType->currentText(), true, true);
        }
        else if (interfaceTypeName == "canProtocol")
        {
            emit LOG_D("Can protocol type changed to: " + protocolType->currentText(), true, true);
        }
    }
}

void DataTerminal::listenInterface()
{
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();

    QPushButton *btn = (QPushButton *)obj;
    if (btn->isChecked())
    {
        if (interfaceTypeName == "klineProtocol")
        {
            emit LOG_I("Start listening K-Line interface", true, true);
        }
        else
        {
            emit LOG_I("Start listening CANbus interface", true, true);
        }
    }
    else if (interfaceTypeName == "klineProtocol")
    {
        emit LOG_I("Stop listening K-Line interface", true, true);
    }
    else
    {
        emit LOG_I("Stop listening CANbus interface", true, true);
    }
}

void DataTerminal::sendToInterface()
{
    bool serialOk = true;
    bool ok = false;
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();
    emit LOG_D("Send data to interface", true, true);

    QFile file;
    QString msg;
    std::vector<std::string> scriptLines;

    if (interfaceTypeName.startsWith("sendKlineMessage"))
    {
        msg = ui->klineMsgToSend->text();
    }
    else
    {
        msg = ui->canMsgToSend->text();
    }

    if (msg == "")
    {
        emit LOG_E("Add message bytes or file to send", true, true);
        QMessageBox::warning(this, tr("Data terminal"), "Add message bytes or file to send");
        return;
    }

    if (msg.at(0) != '.' && msg.at(0) != '/')
    {
        emit LOG_D("Read message from lineedit", true, true);
        scriptLines.push_back(msg.toStdString());
    }
    else
    {
        emit LOG_D("Read message from file", true, true);
        QFile file_local(msg);
        if (!file_local.open(QIODevice::ReadOnly))
        {
            emit LOG_E("Unable to open datastream file '" + file_local.fileName() + "' for reading", true, true);
            QMessageBox::warning(this, tr("Data terminal"),
                                 "Unable to open datastream file '" + file_local.fileName() + "' for reading");
            return;
        }
        QTextStream in(&file_local);
        while (!in.atEnd())
        {
            QString line = in.readLine();
            scriptLines.push_back(line.toStdString());
        }
        file_local.close();
    }

    const auto script = fastecu::diagnostics::parse_terminal_script(scriptLines);
    if (!script.has_value())
    {
        const QString detail = QString::fromStdString(script.error().detail);
        emit LOG_E("Invalid script: " + detail, true, true);
        QMessageBox::warning(this, tr("Data terminal"), "Invalid script: " + detail);
        return;
    }

    if (interfaceTypeName.startsWith("sendKlineMessage"))
    {
        emit LOG_D("Send message via K-Line", true, true);

        emit LOG_D("Checking protocol: " + ui->klineProtocol->currentText(), true, true);
        bool iso14230 = false;
        if (ui->klineProtocol->currentText() == "SSM")
        {
            iso14230 = false;
        }
        else if (ui->klineProtocol->currentText() == "iso14230")
        {
            iso14230 = true;
        }
        else
        {
            serialOk = false;
        }
        emit LOG_D("Checking baudrate: " + ui->klineBaudRate->text(), true, true);
        // clang-tidy's DeMorgan rewrite (>= / && / <=  ->  < / || / >) is not value-identical
        // here: QString::toDouble() parses "nan"/"NaN" text to NaN with ok=true, and NaN fails
        // every relational operator, so the flipped form would treat a NaN baud rate as
        // in-range instead of rejecting it.
        // NOLINTNEXTLINE(readability-simplify-boolean-expr)
        if (!(ui->klineBaudRate->text().toDouble() >= 300 && ui->klineBaudRate->text().toDouble() <= 2000000))
        {
            serialOk = false;
        }
        emit LOG_D("Checking tester id: " + ui->klineTesterId->text(), true, true);
        const auto tester = static_cast<std::uint8_t>(ui->klineTesterId->text().toUInt(&ok, 16));
        emit LOG_D("Checking target id: " + ui->klineTargetId->text(), true, true);
        const auto target = static_cast<std::uint8_t>(ui->klineTargetId->text().toUInt(&ok, 16));
        if (serialOk)
        {
            emit LOG_D("All good, setting interface...", true, true);
            emit LOG_D("Opening interface...", true, true);
            const auto opened = link->open(fastecu::diagnostics::KlineLinkConfig{
                .header = fastecu::diagnostics::KlineHeader::None,
                .iso14230_connection = iso14230,
                .baud = qRound(ui->klineBaudRate->text().toDouble()),
                .start_byte = 0x80,
                .tester_id = tester,
                .target_id = target,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }

        QByteArray received;
        for (const auto& step : *script)
        {
            if (const auto *pause = std::get_if<fastecu::diagnostics::TerminalDelayStep>(&step))
            {
                emit LOG_D("Delay " + QString::number(pause->duration.count()) + " ms", true, true);
                delay(static_cast<int>(pause->duration.count()));
                continue;
            }
            QByteArray output = bytes::toQByteArray(std::get<fastecu::diagnostics::TerminalMessageStep>(step).payload);
            if (ui->klineProtocol->currentText() == "SSM")
            {
                output = add_ssm_header(output, ui->klineTesterId->text().toUInt(&ok, 16),
                                        ui->klineTargetId->text().toUInt(&ok, 16), false);
            }

            emit LOG_I("Sent: " + parse_message_to_hex(output), true, true);
            diagnostic_link_io::write(*link, output);
            delay(10);
            received = diagnostic_link_io::read_or_empty(*link, serial_read_short_timeout);
            emit LOG_I("Response: " + parse_message_to_hex(received), true, true);
        }
        std::ignore = link->reset();
    }
    else if (interfaceTypeName.startsWith("sendCanMessage"))
    {
        emit LOG_D("Send message via CAN / iso15765", true, true);

        emit LOG_D("Checking protocol: " + ui->canProtocol->currentText(), true, true);
        bool iso15765 = false;
        if (ui->canProtocol->currentText() == "CAN")
        {
            iso15765 = false;
        }
        else if (ui->canProtocol->currentText() == "iso15765")
        {
            iso15765 = true;
        }
        else
        {
            serialOk = false;
        }
        emit LOG_D("Checking baudrate: " + ui->canBaudRate->text(), true, true);
        // See the K-Line baudrate check above; the same NaN-vs-DeMorgan hazard applies to this
        // field's toDouble() call.
        // NOLINTNEXTLINE(readability-simplify-boolean-expr)
        if (!(ui->canBaudRate->text().toDouble() >= 300 && ui->canBaudRate->text().toDouble() <= 2000000))
        {
            serialOk = false;
        }
        emit LOG_D("Checking CAN ID length: " + ui->canIdLength->currentText(), true, true);
        emit LOG_D("Checking tester id: " + ui->canTesterId->text(), true, true);
        const std::uint32_t source = ui->canTesterId->text().toUInt(&ok, 16);
        emit LOG_D("Checking target id: " + ui->canTargetId->text(), true, true);
        const std::uint32_t destination = ui->canTargetId->text().toUInt(&ok, 16);
        if (serialOk)
        {
            emit LOG_D("All good, setting interface...", true, true);
            emit LOG_D("Opening interface...", true, true);
            const auto opened = link->open(fastecu::diagnostics::CanLinkConfig{
                .iso15765 = iso15765,
                .bitrate = qRound(ui->canBaudRate->text().toDouble()),
                .extended_id = ui->canIdLength->currentIndex() == 1,
                .source_id = source,
                .destination_id = destination,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }

        QByteArray received;
        const unsigned int can_tester_id = ui->canTesterId->text().toUInt(&ok, 16);
        for (const auto& step : *script)
        {
            if (const auto *pause = std::get_if<fastecu::diagnostics::TerminalDelayStep>(&step))
            {
                emit LOG_D("Delay " + QString::number(pause->duration.count()) + " ms", true, true);
                delay(static_cast<int>(pause->duration.count()));
                continue;
            }
            const auto& payload = std::get<fastecu::diagnostics::TerminalMessageStep>(step).payload;
            if (ui->canProtocol->currentText() == "CAN" && payload.size() > 8)
            {
                emit LOG_E("CAN message too long (8 message bytes)", true, true);
                QMessageBox::warning(this, tr("CAN message"),
                                     "CAN message too long (use 4 ID bytes + 8 message bytes)");
            }
            QByteArray output;
            for (const unsigned int shift : {24U, 16U, 8U, 0U})
            {
                output.append(static_cast<char>((can_tester_id >> shift) & 0xffU));
            }
            output.append(bytes::toQByteArray(payload));
            emit LOG_I("Sent: " + parse_message_to_hex(output), true, true);
            diagnostic_link_io::write(*link, output);
            delay(10);
            received = diagnostic_link_io::read_or_empty(*link, serial_read_short_timeout);
            emit LOG_I("Response: " + parse_message_to_hex(received), true, true);
        }
        std::ignore = link->reset();
    }
}

/*
 * Add SSM header to message
 *
 * @return parsed message
 */
QByteArray DataTerminal::add_ssm_header(QByteArray output, uint8_t tester_id, uint8_t target_id, bool dec_0x100)
{
    uint8_t length = output.length();

    emit LOG_D("Append SSM header for message: " + parse_message_to_hex(output) + " length: " + QString::number(length),
               true, true);
    output.insert(0, static_cast<char>(0x80));
    output.insert(1, static_cast<char>(target_id));
    output.insert(2, static_cast<char>(tester_id));
    output.insert(3, static_cast<char>(length));

    output.append(static_cast<char>(calculate_checksum(output, dec_0x100)));

    emit LOG_D("Constructed SSM message: " + parse_message_to_hex(output), true, true);
    return output;
}

/*
 * Calculate SSM checksum to message
 *
 * @return 8-bit checksum
 */
uint8_t DataTerminal::calculate_checksum(const QByteArray& output, bool dec_0x100)
{
    uint8_t checksum = 0;

    for (qsizetype i = 0; i < output.length(); i++)
    {
        checksum += (uint8_t)output.at(i);
    }

    if (dec_0x100)
    {
        checksum = (uint8_t)(0x100 - checksum);
    }

    return checksum;
}

/*
 * Parse QByteArray to readable form
 *
 * @return parsed message
 */
QString DataTerminal::parse_message_to_hex(const QByteArray& received)
{
    QString msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}

void DataTerminal::delay(int timeout)
{
    QTime dieTime = QTime::currentTime().addMSecs(timeout);
    while (QTime::currentTime() < dieTime)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
    }
}
