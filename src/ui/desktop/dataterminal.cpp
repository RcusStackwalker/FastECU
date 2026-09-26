#include "dataterminal.h"
#include "src/ui/desktop/diagnostic_link_io.h"

#include <QFile>

#include <cstdint>

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

// Legacy protocol framing mixes signed QByteArray::at()/toUInt() results into
// bitwise arithmetic; tracked in docs/tech-debt.md "Convert suppressed
// signed-bitwise arithmetic to unsigned operands".
// NOLINTBEGIN(bugprone-signed-bitwise)
void DataTerminal::sendToInterface()
{
    bool serialOk = true;
    bool ok = false;
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();
    emit LOG_D("Send data to interface", true, true);

    QFile file;
    QString msg;
    QStringList msgList;

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
        msgList.append(msg);
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
            msgList.append(line);
        }
        file_local.close();
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
                .baud = ui->klineBaudRate->text().toInt(),
                .start_byte = 0x80,
                .tester_id = tester,
                .target_id = target,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }

        QStringList msg_local; // = ui->klineMsgToSend->text().split(" ");
        QByteArray output;
        QByteArray received;
        int rspDelay = 10;
        for (int j = 0; j < msgList.length(); j++)
        {
            output.clear();
            received.clear();
            rspDelay = 10;
            if (!msgList.at(j).startsWith("delay"))
            {
                msg_local = msgList.at(j).split(" ");
                for (int i = 0; i < msg_local.length(); i++)
                {
                    output.append(msg_local.at(i).toUInt(&ok, 16));
                }
                if (ui->klineProtocol->currentText() == "SSM")
                {
                    output = add_ssm_header(output, ui->klineTesterId->text().toUInt(&ok, 16),
                                            ui->klineTargetId->text().toUInt(&ok, 16), false);
                }

                emit LOG_I("Sent: " + parse_message_to_hex(output), true, true);
            }
            if (msgList.length() > (j + 1))
            {
                if (msgList.at(j + 1).startsWith("delay"))
                {
                    emit LOG_D("Set delay", true, true);
                    delay(msgList.at(j + 1).split(")").at(1).split("(").at(0).toUInt());
                    j++;
                }
            }
            diagnostic_link_io::write(*link, output);
            delay(rspDelay);
            received = diagnostic_link_io::read_or_empty(*link, serial_read_short_timeout);
            emit LOG_I("Response: " + parse_message_to_hex(received), true, true);
        }
        static_cast<void>(link->reset());
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
                .bitrate = ui->canBaudRate->text().toInt(),
                .extended_id = ui->canIdLength->currentIndex() == 1,
                .source_id = source,
                .destination_id = destination,
            });
            if (!opened.has_value())
            {
                emit LOG_E("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
            }
        }

        QStringList msg_local; // = ui->canMsgToSend->text().split(" ");
        QByteArray output;
        QByteArray received;
        int rspDelay = 100;
        for (int j = 0; j < msgList.length(); j++)
        {
            output.clear();
            received.clear();
            rspDelay = 10;
            if (!msgList.at(j).startsWith("delay"))
            {
                msg_local = msgList.at(j).split(" ");
                if (ui->canProtocol->currentText() == "CAN")
                {
                    if (msg_local.length() > 8)
                    {
                        emit LOG_E("CAN message too long (8 message bytes)", true, true);
                        QMessageBox::warning(this, tr("CAN message"),
                                             "CAN message too long (use 4 ID bytes + 8 message bytes)");
                    }
                }
                for (int i = 3; i >= 0; i--)
                {
                    output.append(((ui->canTesterId->text().toUInt(&ok, 16) >> (i * 8)) & 0xff));
                }
                for (int i = 0; i < msg_local.length(); i++)
                {
                    output.append(msg_local.at(i).toUInt(&ok, 16));
                }
                emit LOG_I("Sent: " + parse_message_to_hex(output), true, true);
            }
            if (msgList.length() > (j + 1))
            {
                if (msgList.at(j + 1).startsWith("delay"))
                {
                    emit LOG_D("Set delay", true, true);
                    rspDelay = msgList.at(j + 1).split(")").at(1).split("(").at(0).toUInt();
                    j++;
                }
            }
            diagnostic_link_io::write(*link, output);

            delay(rspDelay);
            received = diagnostic_link_io::read_or_empty(*link, serial_read_short_timeout);
            emit LOG_I("Response: " + parse_message_to_hex(received), true, true);
        }
        static_cast<void>(link->reset());
    }
}
// NOLINTEND(bugprone-signed-bitwise)

/*
 * Add SSM header to message
 *
 * @return parsed message
 */
// NOLINTBEGIN(bugprone-signed-bitwise): see the note above sendToInterface().
QByteArray DataTerminal::add_ssm_header(QByteArray output, uint8_t tester_id, uint8_t target_id, bool dec_0x100)
{
    uint8_t length = output.length();

    emit LOG_D("Append SSM header for message: " + parse_message_to_hex(output) + " length: " + QString::number(length),
               true, true);
    output.insert(0, (uint8_t)0x80);
    output.insert(1, target_id & 0xFF);
    output.insert(2, tester_id & 0xFF);
    output.insert(3, length);

    output.append(calculate_checksum(output, dec_0x100));

    emit LOG_D("Constructed SSM message: " + parse_message_to_hex(output), true, true);
    return output;
}
// NOLINTEND(bugprone-signed-bitwise)

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
