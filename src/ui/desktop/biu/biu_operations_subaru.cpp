#include "biu_operations_subaru.h"
#include <ui_biu_operations_subaru.h>

#include <algorithm>
#include <cstddef>
#include <tuple>
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

void BiuOperationsSubaru::closeEvent(QCloseEvent *event)
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
    for (int i = 0; i < selectedItemMsg.length(); i++)
    {
        cmd_.push_back(static_cast<bytes::Byte>(selectedItemMsg.at(i).toUInt(&ok, 16)));
    }

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

void BiuOperationsSubaru::parseBiuMessage(const QByteArray& message)
{
    if (!message.length())
    {
        emit logI("Invalid message received: zero length", true, true);
        return;
    }

    const auto frame = bytes::View(message);
    const bytes::Byte chkSum = bytes::Sum8(frame.first(frame.size() - 1));

    if (((uint8_t)message.at(0) & 0x80U) != 0x80 || (uint8_t)message.at(1) != 0xf0 || (uint8_t)message.at(2) != 0x40)
    {
        emit logI("Invalid message received: invalid header", true, true);
        return;
    }

    if (((uint8_t)message.at(0) & 0x7FU) != static_cast<unsigned>((uint8_t)message.length() - 4))
    {
        emit logI("Invalid message received: invalid length", true, true);
        return;
    }

    if (chkSum != (uint8_t)message.at(message.length() - 1))
    {
        emit logI("Invalid message received: invalid checksum", true, true);
        return;
    }

    if ((uint8_t)message.at(3) == (kConnect + 0x40))
    {
        /*
         * index: 0    1    2    3    4    5    6
         * cmd:   fm+l dest src  0x81 cksm
         * rsp:   fm+l dest src  rply 0xK1 0xK2 cksm
         */

        emit logI("Connection to BIU successful", true, true);
        connection_state_ = kConnected;
        current_command_ = kTesterPresent;
    }
    else if ((uint8_t)message.at(3) == (kDisconnect + 0x40))
    {
        /*
         * index: 0    1    2    3    4    5    6
         * cmd:   fm+l dest src  0x82 cksm
         * rsp:   fm+l dest src  rply cksm
         */

        emit logI("Disconnection from BIU successful", true, true);
        connection_state_ = kNotConnected;
        current_command_ = kNoCommand;
        closeResultsWindows();
    }
    else if ((uint8_t)message.at(3) == (kDtcRead + 0x40))
    {
        /*
         * index: 0    1    2    3    4    5    6
         * cmd:   fm+l dest src  0x18 cksm
         * rsp:   fm+l dest src  rply 0xD1 0xD2 0xD3 cksm
         */

        // QStringList dtc_result;
        QString dtcCode;
        QString byte;

        current_command_ = kTesterPresent;

        int index = 5;

        data_result_->clear();

        if (message.length() >= (index + 4))
        {
            // emit LOG_I("BIU DTC list:", true, true);

            while (index < (message.length() - 1))
            {
                byte = QString("%1").arg((uint8_t)message.at(index) & 0x0fU, 2, 16, QLatin1Char('0'));
                dtcCode = "B" + byte;
                index++;
                byte = QString("%1").arg((uint8_t)message.at(index) & 0xffU, 2, 16, QLatin1Char('0'));
                dtcCode.append(byte);
                index++;
                for (int i = 0; i < biu_dtc_list_.length(); i += 2)
                {
                    if (dtcCode == biu_dtc_list_.at(i))
                    {
                        dtcCode.append(" - " + biu_dtc_list_.at(i + 1));
                    }
                }
                if (message.at(index) == 0)
                {
                    dtcCode.append(" (pending)");
                }
                else
                {
                    dtcCode.append(" (stored)");
                }
                index++;

                if (!dtcCode.isEmpty())
                {
                    // emit LOG_I(dtc_code, true, true);
                    data_result_->append(dtcCode);
                }
            }
        }
        else
        {
            data_result_->append("No BIU DTC found");
            emit logI("No BIU DTC found", true, true);
        }

        biu_ops_subaru_data_dtcs_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_dtcs_);
    }
    else if ((uint8_t)message.at(3) == (kDtcClear + 0x40))
    {
        /*
         * index: 0    1    2    3    4    5    6
         * cmd:   fm+l dest src  0x14 0x40 0x00 cksm
         * rsp:   fm+l dest src  rply 0x40 0x00 cksm
         */

        current_command_ = kTesterPresent;

        emit logI("BIU DTCs successfully cleared", true, true);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kInOutSwitches)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x50 cksm
         * rsp:   fm+l dest src  rply 0x50 0xB1 0xB2 0xBn cksm
         */

        int index = 5;
        int i;

        switch_result_->clear();

        if (message.length() >= (index + 2))
        {
            for (index = 5; index < (message.length() - 1); index++)
            {
                unsigned bitMask = 1;
                for (int bitCounter = 0; bitCounter < 8; bitCounter++)
                {
                    i = ((index - 5) * 8) + bitCounter;
                    switch_result_->append(biu_switch_names_.at(i));
                    if ((uint8_t)message.at(index) & bitMask)
                    {
                        switch_result_->append("ON");
                    }
                    else
                    {
                        switch_result_->append("OFF");
                    }
                    // emit LOG_I(switch_result->at(2 * i) + switch_result->at(2 * i + 1), true, true);
                    bitMask = bitMask << 1U;
                }
            }
        }

        biu_ops_subaru_switches_io_ = updateBiuOpsSubaruSwitchesWindow(biu_ops_subaru_switches_io_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kLightingSwitches)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x51 cksm
         * rsp:   fm+l dest src  rply 0x51 0xB1 0xB2 0xBn cksm
         */

        int index = 5;
        int i;

        switch_result_->clear();

        if (message.length() >= (index + 2))
        {
            for (index = 5; index < (message.length() - 1); index++)
            {
                unsigned bitMask = 1;
                for (int bitCounter = 0; bitCounter < 8; bitCounter++)
                {
                    i = ((index - 5) * 8) + bitCounter;
                    switch_result_->append(biu_lightsw_names_.at(i));
                    if ((uint8_t)message.at(index) & bitMask)
                    {
                        switch_result_->append("ON");
                    }
                    else
                    {
                        switch_result_->append("OFF");
                    }
                    // emit LOG_I(switch_result, true, true);
                    bitMask = bitMask << 1U;
                }
            }
        }

        biu_ops_subaru_switches_lighting_ = updateBiuOpsSubaruSwitchesWindow(biu_ops_subaru_switches_lighting_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kBiuData)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x40 cksm
         * rsp:   fm+l dest src  rply 0x40 0xB1 0xB2 0xBn cksm
         */

        float calcResult;
        QString biuDataResult;
        int index = 5;

        data_result_->clear();

        if (message.length() >= (index + 2))
        {
            for (index = 5; index < (message.length() - 1); index++)
            {
                biuDataResult = biu_data_names_.at((static_cast<qsizetype>(index) - 5) * 2);
                calcResult = (static_cast<float>(static_cast<uint8_t>(message.at(index))) *
                              kBiuDataFactors[static_cast<ptrdiff_t>((index - 5) * 2)]) +
                             kBiuDataFactors[(index - 5) * 2 + 1];
                biuDataResult.append(QString("%1 ").arg(calcResult));
                biuDataResult.append(biu_data_names_.at((index - 5) * 2 + 1));
                data_result_->append(biuDataResult);
                // emit LOG_I(data_result, true, true);
            }
        }

        biu_ops_subaru_data_biu_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_biu_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kCanData)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x41 cksm
         * rsp:   fm+l dest src  rply 0x41 0xB1 0xB2 0xBn cksm
         */

        QString canDataResult;
        float calcResult;

        int item = 0;

        data_result_->clear();

        if (message.length() >= 7)
        {

            // front wheel speed
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = bytes::ReadU16Le(bytes::View(message), 5);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // VDC/ABS latest f-code
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            canDataResult.append(QString("%1 ").arg((uint8_t)message.at(8), 2, 16, QLatin1Char('0')));
            canDataResult.append(QString("%1 ").arg((uint8_t)message.at(7), 2, 16, QLatin1Char('0')));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // Blower fan steps
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = (uint8_t)message.at(9);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // Fuel level resistance
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = bytes::ReadU16Le(bytes::View(message), 10);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // Fuel consumption
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = bytes::ReadU16Le(bytes::View(message), 12);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // engine coolant temp
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = (uint8_t)message.at(14);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // g-force
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = (uint8_t)message.at(15);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // sport shift
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = (uint8_t)message.at(16);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);

            // shift position
            item++;
            canDataResult = can_data_names_.at(static_cast<qsizetype>(item) * 2);
            calcResult = (uint8_t)message.at(17);
            calcResult =
                (calcResult * kCanDataFactors[static_cast<ptrdiff_t>(item * 2)]) + kCanDataFactors[item * 2 + 1];
            canDataResult.append(QString("%1 ").arg(calcResult));
            canDataResult.append(can_data_names_.at(item * 2 + 1));
            data_result_->append(canDataResult);
            // emit LOG_I(can_data_result, true, true);
        }

        biu_ops_subaru_data_can_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_can_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kTimeTempRead)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x52 cksm
         * rsp:   fm+l dest src  rply 0x52 0xB1 0xB2 0xBn cksm
         */

        current_command_ = kTesterPresent;
        QString temp;
        float calcResult;

        data_result_->clear();

        if (message.length() >= 7)
        {

            // room lamp off delay time
            temp = biu_tt_names_.at(0);
            biu_tt_result_->append(static_cast<char>((uint8_t)message.at(5) & 0x03U));
            calcResult = static_cast<float>((uint8_t)message.at(5) & 0x03U);
            if (calcResult == 0)
            {
                temp.append("Normal");
            }
            else if (calcResult == 0x01)
            {
                temp.append("OFF");
            }
            else if (calcResult == 0x02)
            {
                temp.append("Short");
            }
            else if (calcResult == 0x03)
            {
                temp.append("Long");
            }
            temp.append(biu_tt_names_.at(1));
            data_result_->append(temp);
            // emit LOG_I(biu_tt_result, true, true);

            // auto-lock time
            temp = biu_tt_names_.at(2);
            biu_tt_result_->append(static_cast<char>((uint8_t)message.at(6) & 0x07U));
            calcResult = static_cast<float>(((uint8_t)message.at(6) & 0x07U) * 10);
            temp.append(QString("%1 ").arg(calcResult));
            temp.append(biu_tt_names_.at(3));
            data_result_->append(temp);
            // emit LOG_I(biu_tt_result, true, true);

            // outside temp offset
            if (message.length() == 9)
            {
                temp = biu_tt_names_.at(4);
                biu_tt_result_->append(static_cast<char>((uint8_t)message.at(7) & 0x0FU));
                calcResult =
                    static_cast<float>((static_cast<int>((((uint8_t)message.at(7) & 0x0FU) + 4U) & 0x0FU) - 4) * 0.5);
                temp.append(QString("%1 ").arg(calcResult));
                temp.append(biu_tt_names_.at(5));
                data_result_->append(temp);
                // emit LOG_I(biu_tt_result, true, true);
            }
        }

        biu_ops_subaru_data_tt_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_tt_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kOptionsRead)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x53 cksm
         * rsp:   fm+l dest src  rply 0x53 0xB1 0xB2 0xBn cksm
         */

        // QString biu_option_result;

        current_command_ = kTesterPresent;
        switch_result_->clear();

        int index = 5;
        int i;

        if (message.length() >= (index + 2))
        {
            for (index = 5; index < (message.length() - 1); index++)
            {
                unsigned bitMask = 1;
                for (int bitCounter = 0; bitCounter < 8; bitCounter++)
                {
                    i = ((index - 5) * 8) + bitCounter;
                    switch_result_->append(biu_option_names_.at(static_cast<qsizetype>(i) * 3));
                    if ((uint8_t)message.at(index) & bitMask)
                    {
                        switch_result_->append(biu_option_names_.at(i * 3 + 1));
                    }
                    else
                    {
                        switch_result_->append(biu_option_names_.at(i * 3 + 2));
                    }
                    // emit LOG_I(switch_result->at(2 * i) + switch_result->at(2 * i + 1), true, true);
                    bitMask = bitMask << 1U;
                }

                biu_option_result_->append(static_cast<char>((uint8_t)message.at(index)));
            }
        }

        biu_ops_subaru_switches_options_ = updateBiuOpsSubaruSwitchesWindow(biu_ops_subaru_switches_options_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kVdcAbsCondition)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x60 cksm
         * rsp:   fm+l dest src  rply 0x60 0xB1 cksm
         */

        current_command_ = kTesterPresent;
        data_result_->clear();

        int condition = static_cast<int>((uint8_t)message.at(5) & 0x07U);
        data_result_->append("VDC/ABS Condition: " + QString::number(condition));
        // emit LOG_I(data_result, true, true);

        biu_ops_subaru_data_vdcabs_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_vdcabs_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kDestTouchStatus)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x61 cksm
         * rsp:   fm+l dest src  rply 0x61 0xB1 0xB2 cksm
         */

        int condition;
        current_command_ = kTesterPresent;
        data_result_->clear();

        condition = static_cast<int>((uint8_t)message.at(5) & 0x0FU);
        data_result_->append("Destination:    " + QString::number(condition));
        condition = static_cast<int>((uint8_t)message.at(6) & 0x3FU);
        data_result_->append("Touchscreen SW: " + QString::number(condition));
        // emit LOG_I(data_result, true, true);

        biu_ops_subaru_data_dest_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_dest_);
    }
    else if ((uint8_t)message.at(3) == (kInfoRequest + 0x40) && (uint8_t)message.at(4) == kFactoryStatus)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x21 0x54 cksm
         * rsp:   fm+l dest src  rply 0x54 0xB1 cksm
         */

        QString setting;
        current_command_ = kTesterPresent;
        data_result_->clear();

        if ((uint8_t)message.at(5) & 0x01U)
        {
            setting = "Factory";
        }
        else
        {
            setting = "Market";
        }
        data_result_->append("Factory Initial Setting: " + setting);
        // emit LOG_I(data_result, true, true);

        biu_ops_subaru_data_factory_ = updateBiuOpsSubaruDataWindow(biu_ops_subaru_data_factory_);
    }
    else if ((uint8_t)message.at(3) == (kWriteData + 0x40) && (uint8_t)message.at(4) == kTimeTempWrite)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x3E 0x8A 0xD1 0xD2 0xD3 cksm
         * rsp:   fm+l dest src  rply 0x8A cksm
         */

        current_command_ = kTesterPresent;

        emit logI("Setting change successful", true, true);
        biu_tt_result_->clear();
        biu_ops_subaru_input1_->close();
    }
    else if ((uint8_t)message.at(3) == (kWriteData + 0x40) && (uint8_t)message.at(4) == kOptionsWrite)
    {
        /*
         * index: 0    1    2    3    4    5    6    7
         * cmd:   fm+l dest src  0x3E 0x8C 0xD1 0xD2 0xD3 cksm
         * rsp:   fm+l dest src  rply 0x8C cksm
         */

        current_command_ = kTesterPresent;

        emit logI("Setting change successful", true, true);
        biu_option_result_->clear();
        biu_ops_subaru_input2_->close();
    }
    else if ((uint8_t)message.at(3) == 0x7F)
    {
        emit logI("Error response received from BIU", true, true);
    }
}

QString BiuOperationsSubaru::parseMessageToHex(const QByteArray& received)
{
    QByteArray msg;

    for (qsizetype i = 0; i < received.length(); i++)
    {
        msg.append(QString("%1 ").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUtf8());
    }

    return msg;
}

void BiuOperationsSubaru::delay(int timeout)
{
    QTime dieTime = QTime::currentTime().addMSecs(timeout);
    while (QTime::currentTime() < dieTime)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    }
}
