#include "src/ui/desktop/widgets/dataterminal.h"
#include "src/ui/desktop/diagnostic_link_io.h"

#include "src/backend/diagnostics/terminal_script.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

#include <QFile>
#include <QtGlobal>

#include <algorithm>
#include <cstdint>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

DataTerminal::DataTerminal(fastecu::diagnostics::IDiagnosticLink& linkArg, QWidget *parent)
    : QDialog(parent), ui_{std::make_unique<Ui::DataTerminalWindow>()}
{
    ui_->setupUi(this);

    this->link_ = &linkArg;

    // Set initial values
    ui_->klineProtocol->addItem("SSM");
    ui_->klineProtocol->addItem("iso14230");

    ui_->klineBaudRate->setText("4800");

    ui_->klineDataBits->addItem("7");
    ui_->klineDataBits->addItem("8");
    ui_->klineDataBits->addItem("9");
    ui_->klineDataBits->setCurrentIndex(1);

    ui_->klineStopBits->addItem("1");
    ui_->klineStopBits->addItem("2");

    ui_->klineParity->addItem("None");
    ui_->klineParity->addItem("Odd");
    ui_->klineParity->addItem("Even");

    ui_->klineTesterId->setText("F0");
    ui_->klineTargetId->setText("10");

    ui_->canProtocol->addItem("CAN");
    ui_->canProtocol->addItem("iso15765");
    ui_->canProtocol->setCurrentIndex(1);

    ui_->canBaudRate->setText("500000");

    ui_->canIdLength->addItem("11bit");
    ui_->canIdLength->addItem("29bit");

    ui_->canTesterId->setText("7E0");
    ui_->canTargetId->setText("7E8");

    connect(ui_->klineProtocol, SIGNAL(currentIndexChanged(int)), this, SLOT(protocolTypeChanged(int)));
    connect(ui_->klineListen, SIGNAL(clicked(bool)), this, SLOT(listenInterface()));
    connect(ui_->sendKlineMessage, SIGNAL(clicked(bool)), this, SLOT(sendToInterface()));

    connect(ui_->canProtocol, SIGNAL(currentIndexChanged(int)), this, SLOT(protocolTypeChanged(int)));
    connect(ui_->canListen, SIGNAL(clicked(bool)), this, SLOT(listenInterface()));
    connect(ui_->sendCanMessage, SIGNAL(clicked(bool)), this, SLOT(sendToInterface()));

    this->show();
}

DataTerminal::~DataTerminal()
{
}

void DataTerminal::protocolTypeChanged(int)
{
    emit logD("Change protocol type", true, true);
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();

    QComboBox *protocolType = static_cast<QComboBox *>(obj);

    if (protocolType)
    {
        if (interfaceTypeName == "klineProtocol")
        {
            emit logD("K-Line protocol type changed to: " + protocolType->currentText(), true, true);
        }
        else if (interfaceTypeName == "canProtocol")
        {
            emit logD("Can protocol type changed to: " + protocolType->currentText(), true, true);
        }
    }
}

void DataTerminal::listenInterface()
{
    QObject *obj = sender();
    QString interfaceTypeName = obj->objectName();

    QPushButton *btn = static_cast<QPushButton *>(obj);
    if (btn->isChecked())
    {
        if (interfaceTypeName == "klineProtocol")
        {
            emit logI("Start listening K-Line interface", true, true);
        }
        else
        {
            emit logI("Start listening CANbus interface", true, true);
        }
    }
    else if (interfaceTypeName == "klineProtocol")
    {
        emit logI("Stop listening K-Line interface", true, true);
    }
    else
    {
        emit logI("Stop listening CANbus interface", true, true);
    }
}

namespace
{
// QString::toDouble() parses "nan" text to NaN with ok=true; NaN fails both
// comparisons, so it is out of range.
bool baudInRange(const QString& text)
{
    const double baud = text.toDouble();
    return baud >= 300 && baud <= 2000000;
}
} // namespace

void DataTerminal::sendToInterface()
{
    const QString interfaceTypeName = sender()->objectName();
    emit logD("Send data to interface", true, true);
    const bool kline = interfaceTypeName.startsWith("sendKlineMessage");
    const auto script = readScript(kline ? ui_->klineMsgToSend->text() : ui_->canMsgToSend->text());
    if (!script.has_value())
    {
        return;
    }
    if (kline)
    {
        sendKline(*script);
    }
    else if (interfaceTypeName.startsWith("sendCanMessage"))
    {
        sendCan(*script);
    }
}

void DataTerminal::warn(const QString& message)
{
    emit logE(message, true, true);
    QMessageBox::warning(this, tr("Data terminal"), message);
}

// A message starting with '.' or '/' names a script file; anything else is a
// one-line script.
std::optional<std::vector<fastecu::diagnostics::TerminalStep>> DataTerminal::readScript(const QString& msg)
{
    if (msg.isEmpty())
    {
        warn("Add message bytes or file to send");
        return std::nullopt;
    }
    std::vector<std::string> scriptLines;
    if (msg.at(0) != '.' && msg.at(0) != '/')
    {
        emit logD("Read message from lineedit", true, true);
        scriptLines.push_back(msg.toStdString());
    }
    else
    {
        emit logD("Read message from file", true, true);
        QFile file(msg);
        if (!file.open(QIODevice::ReadOnly))
        {
            warn("Unable to open datastream file '" + file.fileName() + "' for reading");
            return std::nullopt;
        }
        QTextStream in(&file);
        while (!in.atEnd())
        {
            scriptLines.push_back(in.readLine().toStdString());
        }
    }

    auto script = fastecu::diagnostics::ParseTerminalScript(scriptLines);
    if (!script.has_value())
    {
        warn("Invalid script: " + QString::fromStdString(script.error().detail));
        return std::nullopt;
    }
    return std::move(*script);
}

template <typename Config> void DataTerminal::openLink(bool settingsOk, const Config& config)
{
    if (!settingsOk)
    {
        return;
    }
    emit logD("All good, setting interface...", true, true);
    emit logD("Opening interface...", true, true);
    if (const auto opened = link_->Open(config); !opened.has_value())
    {
        emit logE("Unable to open interface: " + QString::fromStdString(opened.error().detail), true, true);
    }
}

void DataTerminal::sendKline(const std::vector<fastecu::diagnostics::TerminalStep>& script)
{
    emit logD("Send message via K-Line", true, true);
    const QString protocol = ui_->klineProtocol->currentText();
    emit logD("Checking protocol: " + protocol, true, true);
    emit logD("Checking baudrate: " + ui_->klineBaudRate->text(), true, true);
    const bool settingsOk = (protocol == "SSM" || protocol == "iso14230") && baudInRange(ui_->klineBaudRate->text());
    bool ok = false;
    emit logD("Checking tester id: " + ui_->klineTesterId->text(), true, true);
    const auto tester = static_cast<std::uint8_t>(ui_->klineTesterId->text().toUInt(&ok, 16));
    emit logD("Checking target id: " + ui_->klineTargetId->text(), true, true);
    const auto target = static_cast<std::uint8_t>(ui_->klineTargetId->text().toUInt(&ok, 16));
    openLink(settingsOk, fastecu::diagnostics::KlineLinkConfig{
                             .header = fastecu::diagnostics::KlineHeader::kNone,
                             .iso14230_connection = protocol == "iso14230",
                             .baud = qRound(ui_->klineBaudRate->text().toDouble()),
                             .start_byte = 0x80,
                             .tester_id = tester,
                             .target_id = target,
                         });

    runScript(script,
              [this, &ok](const bytes::Bytes& payload)
              {
                  QByteArray output = bytes::ToQByteArray(payload);
                  if (ui_->klineProtocol->currentText() == "SSM")
                  {
                      output = addSsmHeader(output, static_cast<uint8_t>(ui_->klineTesterId->text().toUInt(&ok, 16)),
                                            static_cast<uint8_t>(ui_->klineTargetId->text().toUInt(&ok, 16)), false);
                  }
                  return output;
              });
}

void DataTerminal::sendCan(const std::vector<fastecu::diagnostics::TerminalStep>& script)
{
    emit logD("Send message via CAN / iso15765", true, true);
    const QString protocol = ui_->canProtocol->currentText();
    emit logD("Checking protocol: " + protocol, true, true);
    emit logD("Checking baudrate: " + ui_->canBaudRate->text(), true, true);
    const bool settingsOk = (protocol == "CAN" || protocol == "iso15765") && baudInRange(ui_->canBaudRate->text());
    bool ok = false;
    emit logD("Checking CAN ID length: " + ui_->canIdLength->currentText(), true, true);
    emit logD("Checking tester id: " + ui_->canTesterId->text(), true, true);
    const std::uint32_t source = ui_->canTesterId->text().toUInt(&ok, 16);
    emit logD("Checking target id: " + ui_->canTargetId->text(), true, true);
    const std::uint32_t destination = ui_->canTargetId->text().toUInt(&ok, 16);
    openLink(settingsOk, fastecu::diagnostics::CanLinkConfig{
                             .iso15765 = protocol == "iso15765",
                             .bitrate = qRound(ui_->canBaudRate->text().toDouble()),
                             .extended_id = ui_->canIdLength->currentIndex() == 1,
                             .source_id = source,
                             .destination_id = destination,
                         });

    runScript(script,
              [this, source](const bytes::Bytes& payload)
              {
                  if (ui_->canProtocol->currentText() == "CAN" && payload.size() > 8)
                  {
                      emit logE("CAN message too long (8 message bytes)", true, true);
                      QMessageBox::warning(this, tr("CAN message"),
                                           "CAN message too long (use 4 ID bytes + 8 message bytes)");
                  }
                  QByteArray output;
                  for (const unsigned int shift : {24U, 16U, 8U, 0U})
                  {
                      output.append(static_cast<char>((source >> shift) & 0xffU));
                  }
                  output.append(bytes::ToQByteArray(payload));
                  return output;
              });
}

// Sends each message step as `encode` frames it and logs the response; a
// delay step pauses. The link is reset afterwards.
void DataTerminal::runScript(const std::vector<fastecu::diagnostics::TerminalStep>& script,
                             const std::function<QByteArray(const bytes::Bytes&)>& encode)
{
    for (const auto& step : script)
    {
        if (const auto *pause = std::get_if<fastecu::diagnostics::TerminalDelayStep>(&step))
        {
            emit logD("Delay " + QString::number(pause->duration.count()) + " ms", true, true);
            delay(static_cast<int>(pause->duration.count()));
            continue;
        }
        const QByteArray output = encode(std::get<fastecu::diagnostics::TerminalMessageStep>(step).payload);
        emit logI("Sent: " + parseMessageToHex(output), true, true);
        diagnostic_link_io::write(*link_, output);
        delay(10);
        const QByteArray received = diagnostic_link_io::readOrEmpty(*link_, serial_read_short_timeout_);
        emit logI("Response: " + parseMessageToHex(received), true, true);
    }
    std::ignore = link_->Reset();
}

/*
 * Add SSM header to message
 *
 * @return parsed message
 */
QByteArray DataTerminal::addSsmHeader(QByteArray output, uint8_t testerId, uint8_t targetId, bool dec0x100)
{
    uint8_t length = static_cast<uint8_t>(output.length());

    emit logD("Append SSM header for message: " + parseMessageToHex(output) + " length: " + QString::number(length),
              true, true);
    output.insert(0, static_cast<char>(0x80));
    output.insert(1, static_cast<char>(targetId));
    output.insert(2, static_cast<char>(testerId));
    output.insert(3, static_cast<char>(length));

    output.append(static_cast<char>(calculateChecksum(output, dec0x100)));

    emit logD("Constructed SSM message: " + parseMessageToHex(output), true, true);
    return output;
}

/*
 * Calculate SSM checksum to message
 *
 * @return 8-bit checksum
 */
uint8_t DataTerminal::calculateChecksum(const QByteArray& output, bool dec0x100)
{
    uint8_t checksum = std::ranges::fold_left(output, uint8_t{0}, [](uint8_t sum, char byte)
                                              { return static_cast<uint8_t>(sum + static_cast<uint8_t>(byte)); });

    if (dec0x100)
    {
        checksum = static_cast<uint8_t>(0x100 - checksum);
    }

    return checksum;
}

/*
 * Parse QByteArray to readable form
 *
 * @return parsed message
 */
QString DataTerminal::parseMessageToHex(const QByteArray& received)
{
    QString msg;

    for (int i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg(static_cast<uint8_t>(received.at(i)), 2, 16, QLatin1Char('0')).toUtf8());
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
