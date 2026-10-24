#include "biu_operations_subaru.h"
#include <ui_biu_operations_subaru.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <tuple>
#include <utility>
#include "src/algorithms/protocol/biu/subaru_biu_frame.h"
#include "src/ui/desktop/diagnostic_link_io.h"
#include "src/platform/desktop/common/bytes/qt_bytes.h"

BiuOperationsSubaru::BiuOperationsSubaru(fastecu::diagnostics::IDiagnosticLink& linkArg, QWidget *parent)
    : QDialog(parent), ui_{std::make_unique<Ui::BiuOperationsSubaruWindow>()}
{
    ui_->setupUi(this);

    ui_->progressbar->hide();

    this->link_ = &linkArg;

    biu_ops_subaru_switches_io_ = nullptr;
    biu_ops_subaru_switches_lighting_ = nullptr;
    biu_ops_subaru_switches_options_ = nullptr;
    biu_ops_subaru_data_dtcs_ = nullptr;
    biu_ops_subaru_data_biu_ = nullptr;
    biu_ops_subaru_data_can_ = nullptr;
    biu_ops_subaru_data_tt_ = nullptr;
    biu_ops_subaru_data_vdcabs_ = nullptr;
    biu_ops_subaru_data_dest_ = nullptr;
    biu_ops_subaru_data_factory_ = nullptr;

    counter_ = 0;
    biu_tt_result_ = new QByteArray();
    biu_option_result_ = new QByteArray();
    switch_result_ = new QStringList();
    data_result_ = new QStringList();
    keep_alive_timer_ = new QTimer(this);
    keep_alive_timer_->setInterval(1000);
    current_command_ = kNoCommand;
    connection_state_ = kNotConnected;

    for (int i = 0; i < biu_messages_.length(); i += 2)
    {
        ui_->msg_combo_box->addItem(biu_messages_.at(i));
    }

    connect(ui_->send_msg, SIGNAL(clicked(bool)), this, SLOT(parseBiuCmd()));
    connect(keep_alive_timer_, SIGNAL(timeout()), this, SLOT(keepAlive()));

    emit logI("BIU started", true, true);

    this->show();
}

BiuOperationsSubaru::~BiuOperationsSubaru()
{
    keep_alive_timer_->stop();
    /*
    delete keep_alive_timer;
    delete biu_tt_result;
    delete biu_option_result;
    delete switch_result;
    delete data_result;

    delete biuOpsSubaruSwitchesIo;
    delete biuOpsSubaruSwitchesLighting;
    delete biuOpsSubaruSwitchesOptions;
    delete biuOpsSubaruDataDtcs;
    delete biuOpsSubaruDataBiu;
    delete biuOpsSubaruDataCan;
    delete biuOpsSubaruDataTt;
    delete biuOpsSubaruDataVdcabs;
    delete biuOpsSubaruDataDest;
    delete biuOpsSubaruDataFactory;
    delete biuOpsSubaruInput1;
    //delete biuOpsSubaruInput2;
*/
}

BiuOpsSubaruSwitches *BiuOperationsSubaru::updateBiuOpsSubaruSwitchesWindow(BiuOpsSubaruSwitches *biuOpsSubaruSwitches)
{
    if (biuOpsSubaruSwitches == nullptr)
    {
        biuOpsSubaruSwitches = new BiuOpsSubaruSwitches(switch_result_);
        biuOpsSubaruSwitches->show();
    }
    else
    {
        if (!biuOpsSubaruSwitches->isVisible())
        {
            biuOpsSubaruSwitches->show();
        }
        biuOpsSubaruSwitches->updateSwitchResults(switch_result_);
    }

    return biuOpsSubaruSwitches;
}

BiuOpsSubaruData *BiuOperationsSubaru::updateBiuOpsSubaruDataWindow(BiuOpsSubaruData *biuOpsSubaruData)
{
    if (biuOpsSubaruData == nullptr)
    {
        biuOpsSubaruData = new BiuOpsSubaruData(data_result_);
        biuOpsSubaruData->show();
    }
    else
    {
        if (!biuOpsSubaruData->isVisible())
        {
            biuOpsSubaruData->show();
        }
        biuOpsSubaruData->updateDataResults(data_result_);
    }

    return biuOpsSubaruData;
}

void BiuOperationsSubaru::closeResultsWindows()
{
    if (biu_ops_subaru_switches_io_ != nullptr)
    {
        biu_ops_subaru_switches_io_->hide();
    }
    if (biu_ops_subaru_switches_lighting_ != nullptr)
    {
        biu_ops_subaru_switches_lighting_->hide();
    }
    if (biu_ops_subaru_switches_options_ != nullptr)
    {
        biu_ops_subaru_switches_options_->hide();
    }
    if (biu_ops_subaru_data_dtcs_ != nullptr)
    {
        biu_ops_subaru_data_dtcs_->hide();
    }
    if (biu_ops_subaru_data_biu_ != nullptr)
    {
        biu_ops_subaru_data_biu_->hide();
    }
    if (biu_ops_subaru_data_can_ != nullptr)
    {
        biu_ops_subaru_data_can_->hide();
    }
    if (biu_ops_subaru_data_tt_ != nullptr)
    {
        biu_ops_subaru_data_tt_->hide();
    }
    if (biu_ops_subaru_data_vdcabs_ != nullptr)
    {
        biu_ops_subaru_data_vdcabs_->hide();
    }
    if (biu_ops_subaru_data_dest_ != nullptr)
    {
        biu_ops_subaru_data_dest_->hide();
    }
    if (biu_ops_subaru_data_factory_ != nullptr)
    {
        biu_ops_subaru_data_factory_->hide();
    }
}

void BiuOperationsSubaru::closeEvent(QCloseEvent *)
{
    qDebug() << "Closing BIU log window";
    keep_alive_timer_->stop();
    if (biu_ops_subaru_switches_io_ != nullptr)
    {
        biu_ops_subaru_switches_io_->close();
    }
    if (biu_ops_subaru_switches_lighting_ != nullptr)
    {
        biu_ops_subaru_switches_lighting_->close();
    }
    if (biu_ops_subaru_switches_options_ != nullptr)
    {
        biu_ops_subaru_switches_options_->close();
    }
    if (biu_ops_subaru_data_dtcs_ != nullptr)
    {
        biu_ops_subaru_data_dtcs_->close();
    }
    if (biu_ops_subaru_data_biu_ != nullptr)
    {
        biu_ops_subaru_data_biu_->close();
    }
    if (biu_ops_subaru_data_can_ != nullptr)
    {
        biu_ops_subaru_data_can_->close();
    }
    if (biu_ops_subaru_data_tt_ != nullptr)
    {
        biu_ops_subaru_data_tt_->close();
    }
    if (biu_ops_subaru_data_vdcabs_ != nullptr)
    {
        biu_ops_subaru_data_vdcabs_->close();
    }
    if (biu_ops_subaru_data_dest_ != nullptr)
    {
        biu_ops_subaru_data_dest_->close();
    }
    if (biu_ops_subaru_data_factory_ != nullptr)
    {
        biu_ops_subaru_data_factory_->close();
    }
}

void BiuOperationsSubaru::keepAlive()
{
    if (current_command_ == kTesterPresent)
    {
        output_ = biu_subaru::KeepAliveRequest();
    }

    sendBiuMsg();
}

void BiuOperationsSubaru::parseBiuCmd()
{

    QString selectedItemText = ui_->msg_combo_box->currentText();
    QStringList selectedItemMsg;

    bool cmdReady = true;
    bool ok = false;

    if (selectedItemText != "Custom")
    {
        for (int i = 0; i < biu_messages_.length(); i += 2)
        {
            if (selectedItemText == biu_messages_.at(i))
            {
                selectedItemMsg = biu_messages_.at(i + 1).split(",");
            }
        }
    }
    else
    {
        if (!ui_->msg_line_edit->text().isEmpty())
        {
            selectedItemMsg = ui_->msg_line_edit->text().split(",");
        }
    }

    cmd_.clear();
    std::ranges::transform(selectedItemMsg, std::back_inserter(cmd_),
                           [&ok](const QString& item) { return static_cast<bytes::Byte>(item.toUInt(&ok, 16)); });

    if (selectedItemText == "SET:  Times & Temps")
    {
        if (biu_tt_result_->length() > 0)
        {
            // emit LOG_I("TT selected", true, true);
            biu_ops_subaru_input1_ = new BiuOpsSubaruInput1(biu_tt_result_);
            connect(biu_ops_subaru_input1_, SIGNAL(sendBiuSetting1(QByteArray)), this,
                    SLOT(prepareBiuSetCmd(QByteArray)));
            biu_ops_subaru_input1_->show();
        }
        else
        {
            emit logI("Read data before attempting change", true, true);
        }

        cmdReady = false;
    }

    if (selectedItemText == "SET:  Car options")
    {
        if (biu_option_result_->length() > 0)
        {
            biu_ops_subaru_input2_ = new BiuOpsSubaruInput2(&biu_option_names_, biu_option_result_);
            connect(biu_ops_subaru_input2_, SIGNAL(sendBiuSetting2(QByteArray)), this,
                    SLOT(prepareBiuSetCmd(QByteArray)));
            biu_ops_subaru_input2_->show();
        }
        else
        {
            emit logI("Read data before attempting change", true, true);
        }

        cmdReady = false;
    }

    if (cmdReady)
    {
        current_command_ = cmd_[0];
        if (current_command_ == kInfoRequest)
        {
            current_command_ = cmd_[1];
        }

        prepareBiuMsg();
    }
    else
    {
        current_command_ = kTesterPresent;
    }
}

void BiuOperationsSubaru::prepareBiuSetCmd(const QByteArray& cmdSettings)
{
    const auto settings = bytes::View(cmdSettings);
    if (cmd_.size() < 2 + settings.size())
    {
        cmd_.resize(2 + settings.size());
    }
    std::copy(settings.begin(), settings.end(), cmd_.begin() + 2);

    current_command_ = cmd_[0];
    if (current_command_ == kWriteData)
    {
        current_command_ = cmd_[1];
    }

    prepareBiuMsg();
}

void BiuOperationsSubaru::prepareBiuMsg()
{
    output_ = biu_subaru::BuildRequest(cmd_);

    sendBiuMsg();
}

void BiuOperationsSubaru::sendBiuMsg()
{

    keep_alive_timer_->stop();

    QByteArray received;

    if (connection_state_ == kNotConnected && current_command_ != kConnect)
    {
        emit logI("Not connected, can't send command", true, true);
        return;
    }

    if (connection_state_ == kNotConnected && current_command_ == kConnect)
    {
        std::ignore = link_->FastInit(output_);
    }
    else
    {
        std::ignore = link_->Write(output_);
    }

    received = diagnostic_link_io::readOrEmpty(*link_, serial_read_long_timeout_);

    parseBiuMessage(received);

    if (connection_state_ == kConnected)
    {
        keep_alive_timer_->start();
    }
}

namespace
{
std::uint8_t byteAt(const QByteArray& message, qsizetype index)
{
    return static_cast<std::uint8_t>(message.at(index));
}

// Frame layout: [0x80 | payload length] 0xF0 0x40 <reply> <data...> <checksum>.
QString frameError(const QByteArray& message)
{
    if (message.isEmpty())
    {
        return "zero length";
    }
    const auto frame = bytes::View(message);
    const bytes::Byte chkSum = bytes::Sum8(frame.first(frame.size() - 1));
    if ((byteAt(message, 0) & 0x80U) != 0x80 || byteAt(message, 1) != 0xf0 || byteAt(message, 2) != 0x40)
    {
        return "invalid header";
    }
    if ((byteAt(message, 0) & 0x7FU) != static_cast<unsigned>(static_cast<uint8_t>(message.length()) - 4))
    {
        return "invalid length";
    }
    if (chkSum != byteAt(message, message.length() - 1))
    {
        return "invalid checksum";
    }
    return {};
}

// Visits every bit of data bytes 5..n-2, least significant first, numbering
// them from 0.
void forEachBit(const QByteArray& message, const std::function<void(int bit, bool on)>& visit)
{
    if (message.length() < 7)
    {
        return;
    }
    for (int index = 5; index < message.length() - 1; index++)
    {
        for (unsigned bitCounter = 0; bitCounter < 8; bitCounter++)
        {
            visit(((index - 5) * 8) + static_cast<int>(bitCounter),
                  ((static_cast<unsigned>(byteAt(message, index)) >> bitCounter) & 1U) != 0);
        }
    }
}

// "<name><raw * factor + offset> <unit>", with names and factors in pairs per
// item.
QString scaledRow(const QStringList& names, std::span<const float> factors, int item, float raw)
{
    const auto i = static_cast<qsizetype>(item) * 2;
    return names.at(i) +
           QString("%1 ").arg((raw * factors[static_cast<std::size_t>(i)]) + factors[static_cast<std::size_t>(i) + 1]) +
           names.at(i + 1);
}
} // namespace

void BiuOperationsSubaru::parseBiuMessage(const QByteArray& message)
{
    if (const QString error = frameError(message); !error.isEmpty())
    {
        emit logI("Invalid message received: " + error, true, true);
        return;
    }

    switch (byteAt(message, 3))
    {
    case kConnect + 0x40: // rply 0xK1 0xK2
        emit logI("Connection to BIU successful", true, true);
        connection_state_ = kConnected;
        current_command_ = kTesterPresent;
        break;
    case kDisconnect + 0x40:
        emit logI("Disconnection from BIU successful", true, true);
        connection_state_ = kNotConnected;
        current_command_ = kNoCommand;
        closeResultsWindows();
        break;
    case kDtcRead + 0x40: // rply 0xD1 0xD2 0xD3 per DTC
        current_command_ = kTesterPresent;
        showData(dtcRows(message), biu_ops_subaru_data_dtcs_);
        break;
    case kDtcClear + 0x40: // rply 0x40 0x00
        current_command_ = kTesterPresent;
        emit logI("BIU DTCs successfully cleared", true, true);
        break;
    case kInfoRequest + 0x40: // rply <info id> 0xB1 0xB2 0xBn
        parseInfoReply(message);
        break;
    case kWriteData + 0x40: // rply <write id>
        parseWriteReply(message);
        break;
    case 0x7F:
        emit logI("Error response received from BIU", true, true);
        break;
    default:
        break;
    }
}

void BiuOperationsSubaru::parseInfoReply(const QByteArray& message)
{
    switch (byteAt(message, 4))
    {
    case kInOutSwitches:
        showSwitches(message, biu_switch_names_, biu_ops_subaru_switches_io_);
        break;
    case kLightingSwitches:
        showSwitches(message, biu_lightsw_names_, biu_ops_subaru_switches_lighting_);
        break;
    case kBiuData:
    {
        QStringList rows;
        if (message.length() >= 7)
        {
            for (int index = 5; index < message.length() - 1; index++)
            {
                rows.append(scaledRow(biu_data_names_, kBiuDataFactors, index - 5, byteAt(message, index)));
            }
        }
        showData(rows, biu_ops_subaru_data_biu_);
        break;
    }
    case kCanData:
        showData(canDataRows(message), biu_ops_subaru_data_can_);
        break;
    case kTimeTempRead:
        current_command_ = kTesterPresent;
        showData(timeTempRows(message), biu_ops_subaru_data_tt_);
        break;
    case kOptionsRead:
        current_command_ = kTesterPresent;
        showOptions(message);
        break;
    default:
        parseStatusReply(message);
        break;
    }
}

// The one- and two-byte status replies.
void BiuOperationsSubaru::parseStatusReply(const QByteArray& message)
{
    switch (byteAt(message, 4))
    {
    case kVdcAbsCondition:
        current_command_ = kTesterPresent;
        showData({"VDC/ABS Condition: " + QString::number(byteAt(message, 5) & 0x07U)}, biu_ops_subaru_data_vdcabs_);
        break;
    case kDestTouchStatus:
        current_command_ = kTesterPresent;
        showData({"Destination:    " + QString::number(byteAt(message, 5) & 0x0FU),
                  "Touchscreen SW: " + QString::number(byteAt(message, 6) & 0x3FU)},
                 biu_ops_subaru_data_dest_);
        break;
    case kFactoryStatus:
        current_command_ = kTesterPresent;
        showData({QString("Factory Initial Setting: ") + ((byteAt(message, 5) & 0x01U) ? "Factory" : "Market")},
                 biu_ops_subaru_data_factory_);
        break;
    default:
        break;
    }
}

void BiuOperationsSubaru::parseWriteReply(const QByteArray& message)
{
    const std::uint8_t service = byteAt(message, 4);
    if (service != kTimeTempWrite && service != kOptionsWrite)
    {
        return;
    }
    current_command_ = kTesterPresent;
    emit logI("Setting change successful", true, true);
    if (service == kTimeTempWrite)
    {
        biu_tt_result_->clear();
        biu_ops_subaru_input1_->close();
    }
    else
    {
        biu_option_result_->clear();
        biu_ops_subaru_input2_->close();
    }
}

void BiuOperationsSubaru::showData(const QStringList& rows, BiuOpsSubaruData *& window)
{
    *data_result_ = rows;
    window = updateBiuOpsSubaruDataWindow(window);
}

// Two rows per bit: its name, then ON or OFF.
void BiuOperationsSubaru::showSwitches(const QByteArray& message, const QStringList& names,
                                       BiuOpsSubaruSwitches *& window)
{
    switch_result_->clear();
    forEachBit(message, [&](int bit, bool on) { *switch_result_ << names.at(bit) << (on ? "ON" : "OFF"); });
    window = updateBiuOpsSubaruSwitchesWindow(window);
}

// Two rows per bit: its name, then its set or clear label; the raw bytes are
// kept for the options editor.
void BiuOperationsSubaru::showOptions(const QByteArray& message)
{
    switch_result_->clear();
    forEachBit(message,
               [this](int bit, bool on)
               {
                   *switch_result_ << biu_option_names_.at(static_cast<qsizetype>(bit) * 3)
                                   << biu_option_names_.at(bit * 3 + (on ? 1 : 2));
               });
    if (message.length() >= 7)
    {
        biu_option_result_->append(message.mid(5, message.length() - 6));
    }
    biu_ops_subaru_switches_options_ = updateBiuOpsSubaruSwitchesWindow(biu_ops_subaru_switches_options_);
}

// One row per three-byte DTC: code, known description, pending or stored.
QStringList BiuOperationsSubaru::dtcRows(const QByteArray& message)
{
    if (message.length() < 9)
    {
        emit logI("No BIU DTC found", true, true);
        return {"No BIU DTC found"};
    }
    QStringList rows;
    for (int index = 5; index < (message.length() - 1); index += 3)
    {
        QString dtcCode = "B" + QString("%1").arg(byteAt(message, index) & 0x0fU, 2, 16, QLatin1Char('0')) +
                          QString("%1").arg(byteAt(message, index + 1), 2, 16, QLatin1Char('0'));
        for (int i = 0; i < biu_dtc_list_.length(); i += 2)
        {
            if (dtcCode == biu_dtc_list_.at(i))
            {
                dtcCode.append(" - " + biu_dtc_list_.at(i + 1));
            }
        }
        dtcCode.append(message.at(index + 2) == 0 ? " (pending)" : " (stored)");
        rows.append(dtcCode);
    }
    return rows;
}

QStringList BiuOperationsSubaru::canDataRows(const QByteArray& message) const
{
    if (message.length() < 7)
    {
        return {};
    }
    // Data offset and width of each item; width 0 is the VDC/ABS f-code.
    constexpr std::array<std::pair<int, int>, 9> kFields{{
        {5, 2},  // front wheel speed
        {7, 0},  // VDC/ABS latest f-code
        {9, 1},  // blower fan steps
        {10, 2}, // fuel level resistance
        {12, 2}, // fuel consumption
        {14, 1}, // engine coolant temp
        {15, 1}, // g-force
        {16, 1}, // sport shift
        {17, 1}, // shift position
    }};
    QStringList rows;
    for (int item = 0; item < static_cast<int>(kFields.size()); ++item)
    {
        const auto [offset, width] = kFields[static_cast<std::size_t>(item)];
        if (width == 0)
        {
            rows.append(can_data_names_.at(static_cast<qsizetype>(item) * 2) +
                        QString("%1 ").arg(byteAt(message, offset + 1), 2, 16, QLatin1Char('0')) +
                        QString("%1 ").arg(byteAt(message, offset), 2, 16, QLatin1Char('0')) +
                        can_data_names_.at(item * 2 + 1));
            continue;
        }
        const float raw = width == 2 ? bytes::ReadU16Le(bytes::View(message), static_cast<std::size_t>(offset))
                                     : byteAt(message, offset);
        rows.append(scaledRow(can_data_names_, kCanDataFactors, item, raw));
    }
    return rows;
}

// Room lamp delay and auto-lock time, and the outside temperature offset
// when the BIU reports it; the raw settings are kept for the times editor.
QStringList BiuOperationsSubaru::timeTempRows(const QByteArray& message)
{
    if (message.length() < 7)
    {
        return {};
    }
    static const std::array<QString, 4> kLampDelays{"Normal", "OFF", "Short", "Long"};
    const std::uint8_t lampDelay = byteAt(message, 5) & 0x03U;
    const std::uint8_t autoLock = byteAt(message, 6) & 0x07U;
    biu_tt_result_->append(static_cast<char>(lampDelay));
    biu_tt_result_->append(static_cast<char>(autoLock));
    QStringList rows{biu_tt_names_.at(0) + kLampDelays.at(lampDelay) + biu_tt_names_.at(1),
                     biu_tt_names_.at(2) + QString("%1 ").arg(static_cast<float>(autoLock * 10)) + biu_tt_names_.at(3)};
    if (message.length() == 9)
    {
        const std::uint8_t tempOffset = byteAt(message, 7) & 0x0FU;
        biu_tt_result_->append(static_cast<char>(tempOffset));
        const auto degrees = static_cast<float>((static_cast<int>((tempOffset + 4U) & 0x0FU) - 4) * 0.5);
        rows.append(biu_tt_names_.at(4) + QString("%1 ").arg(degrees) + biu_tt_names_.at(5));
    }
    return rows;
}
