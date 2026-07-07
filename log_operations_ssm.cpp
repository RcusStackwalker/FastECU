#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "serial_port_actions.h"
#include "protocol/mut_dma_memory.h"

using namespace mutdma;

void MainWindow::kline_listener()
{
    QByteArray received;

    // serial->change_port_speed("10400");
    serial->change_port_speed("125000");

    while (haltech_ic7_display_on)
    {
        received = serial->read_serial_data(receive_timeout);
        // emit LOG_D(parse_message_to_hex(received), true, true);
        // delay(5);
    }
}

void MainWindow::canbus_listener()
{
    QByteArray received;

    while (haltech_ic7_display_on)
    {
        received = serial->read_serial_data(receive_timeout);
        // emit LOG_D(parse_message_to_hex(received), true, true);
        // delay(5);
    }
}

bool MainWindow::ecu_init()
{
    if (serial->is_serial_port_open())
    {
        if (!ecu_init_complete)
        {
            if (configValues->flash_protocol_selected_make == "Subaru")
            {
                if (configValues->flash_protocol_selected_log_transport == "CAN" || configValues->flash_protocol_selected_log_transport == "iso15765")
                    ssm_can_init();
                else if (configValues->flash_protocol_selected_log_transport == "K-Line")
                    // serial->fast_init(output);
                    ssm_kline_init();
                else if (configValues->flash_protocol_selected_log_transport == "SSM")
                    ssm_init();
            }
        }
    }
    else
    {
        // emit LOG_D("Connection is not ready!", true, true);
        ecu_init_complete = false;
        ecuid.clear();
    }
    // emit LOG_D("ECU ID check complete", true, true);

    return ecu_init_complete;
}

void MainWindow::ssm_init()
{
    QByteArray output;
    QByteArray received;

    serial->reset_connection();
    serial->set_serial_port_baudrate("1953");
    serial->set_serial_port_parity(QSerialPort::EvenParity);
    serial->open_serial_port();

    emit LOG_D("Using SSM1 protocol", true, true);

    emit LOG_D("Issue read cmd...", true, true);
    received.clear();
    output.clear();
    output.append((uint8_t)0x78);
    output.append((uint8_t)0x12);
    output.append((uint8_t)0x34);
    output.append((uint8_t)0x00);
    serial->write_serial_data_echo_check(output);
    // delay(1000);
    for (int i = 0; i < 10; i++)
    {
        received.append(serial->read_serial_data(500));
    }
    emit LOG_D("Received: " + parse_message_to_hex(received), true, true);
    // if (received.length() > 0)
    //     emit LOG_D("Something received " + parse_message_to_hex(received), true, true);
    // else
    //     emit LOG_D("No response...", true, true);

    emit LOG_D("Issue init cmd...", true, true);
    received.clear();
    output.clear();
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x46);
    output.append((uint8_t)0x48);
    output.append((uint8_t)0x49);
    serial->write_serial_data(output);

    emit LOG_D("Init sent, delaying 2s...", true, true);
    // delay(2000);
    for (int i = 0; i < 2; i++)
    {
        received.append(serial->read_serial_data(500));
    }
    emit LOG_D("Received: " + parse_message_to_hex(received), true, true);
    // emit LOG_D("Checking response...";
    // received = serial->read_serial_data(receive_timeout);
    received.clear();
    output.clear();
    output.append((uint8_t)0x12);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    serial->write_serial_data(output);
    received.append(serial->read_serial_data(500));

    if (received.length() > 0)
    {
        emit LOG_D("Something received " + parse_message_to_hex(received), true, true);

        if (received.length() == (uint8_t)received.at(3) + 5)
        {
            ecu_init_complete = true;
            ecuid = parse_ecuid(received);
            emit LOG_D("ECU ID: " + ecuid, true, true);
            parse_log_value_list(received, "SSM");
            if (ecuid == "")
                set_status_bar_label(true, false, "");
            else
                set_status_bar_label(true, true, ecuid);

            received = serial->read_serial_data(100);
            while (received.length() > 0)
            {
                ecu_init_complete = true;
                ecuid = parse_ecuid(received);
                parse_log_value_list(received, "SSM");

                received = serial->read_serial_data(100);
                while (received.length() > 0)
                {
                    received = serial->read_serial_data(100);
                }
            }
        }
    }
    else
        emit LOG_D("No response...", true, true);
}

void MainWindow::ssm_kline_init()
{
    QByteArray output;
    QByteArray received;
    int loopcount = 0;

    // emit LOG_D("ECU K-Line INIT";
    if (!ecu_init_started)
    {
        ecu_init_started = true;

        emit LOG_D("K-Line SSM init with BF", true, true);
        output.clear();
        output.append((uint8_t)0xBF);
        serial->write_serial_data_echo_check(add_ssm_header(output, false));
        emit LOG_D("K-Line SSM init sent", true, true);
        delay(200);
        received = serial->read_serial_data(serial_read_short_timeout);
        emit LOG_D("K-Line SSM init response: " + parse_message_to_hex(received), true, true);
        while (received.length() < 4 && loopcount < 10)
        {
            received.append(serial->read_serial_data(50));
            loopcount++;
        }
        if (received.length() < 4)
            return;

        emit LOG_D("SSM Init response header received", true, true);
        loopcount = 0;
        while (received.length() < (uint8_t)received.at(3) + 5 && loopcount < 10)
        {
            received.append(serial->read_serial_data(50));
            loopcount++;
        }
        if (received.length() < (uint8_t)received.at(3) + 5)
            return;

        emit LOG_D("ECU INIT length: " + QString::number((uint8_t)received.at(3)) + " " + QString::number(received.length()), true, true);
        if (received.length() >= (uint8_t)received.at(3) + 5)
        {
            ecu_init_complete = true;
            // set_status_bar_label(true, true, ecuid);
            ecuid = parse_ecuid(received);
            emit LOG_D("ECU ID: " + ecuid, true, true);
            parse_log_value_list(received, "SSM");
            // emit LOG_D("ECU ID: " + ecuid, true, true);
            if (ecuid == "")
                set_status_bar_label(true, false, "");
            else
                set_status_bar_label(true, true, ecuid);
            return;
        }
        /*
        received.append(serial->read_serial_data(100));
        while(received.length() > 0)
        {
            //emit LOG_D("ECU ID: " + parse_ecuid(received), true, true);
            ecu_init_complete = true;
            //set_status_bar_label(true, true, ecuid);
            ecuid = parse_ecuid(received);
            parse_log_value_list(received, "SSM");

            received = serial->read_serial_data(100);
            while(received.length() > 0)
            {
                received = serial->read_serial_data(100);
            }
        }
*/
    }
    ecu_init_started = false;
}

void MainWindow::ssm_can_init()
{
    QByteArray output;
    QByteArray received;
    uint32_t control_unit_addr = 0;

    if (ecu_radio_button->isChecked())
        control_unit_addr = 0x7E0;
    else if (tcu_radio_button->isChecked())
        control_unit_addr = 0x7E1;

    const uint8_t cu_addr_b3 = (uint8_t)((control_unit_addr >> 24) & 0xFF);
    const uint8_t cu_addr_b2 = (uint8_t)((control_unit_addr >> 16) & 0xFF);
    const uint8_t cu_addr_b1 = (uint8_t)((control_unit_addr >> 8) & 0xFF);
    const uint8_t cu_addr_b0 = (uint8_t)(control_unit_addr & 0xFF);

    emit LOG_D(QString(Q_FUNC_INFO) + " " + configValues->flash_protocol_selected_log_protocol, true, true);

    if (configValues->flash_protocol_selected_log_protocol == "CAN")
        received = serial->read_serial_data(receive_timeout);
    else if (configValues->flash_protocol_selected_log_transport == "iso15765")
    {
        // Set serial port
        serial->reset_connection();
        serial->set_is_iso14230_connection(false);
        serial->set_add_iso14230_header(false);
        serial->set_is_can_connection(false);
        serial->set_is_iso15765_connection(true);
        serial->set_is_29_bit_id(false);
        serial->set_can_speed("500000");
        serial->set_iso15765_source_address(control_unit_addr);
        serial->set_iso15765_destination_address(0x7E8);
        // Open serial port
        serial->open_serial_port();

        output.clear();
        output.append(cu_addr_b3);
        output.append(cu_addr_b2);
        output.append(cu_addr_b1);
        output.append(cu_addr_b0);
        output.append((uint8_t)0x22);
        output.append((uint8_t)0xF1);
        output.append((uint8_t)0x82);
        serial->write_serial_data_echo_check(output);
        received = serial->read_serial_data(100);
        if (received.length() > 7 &&
            (uint8_t)received.at(4) == 0x62 &&
            (uint8_t)received.at(5) == 0xF1 &&
            (uint8_t)received.at(6) == 0x82)
        {
            ecu_init_complete = true;
            received = received.remove(0, 7);
            QString msg;
            for (int i = 0; i < received.length(); i++)
            {
                msg.append(QString("%1").arg((uint8_t)received.at(i), 2, 16, QLatin1Char('0')).toUpper());
            }
            emit LOG_D("ECU ID " + msg, true, true);
            set_status_bar_label(true, true, msg);
        }
    }
}

void MainWindow::parse_log_value_list(QByteArray received, QString protocol)
{
    uint16_t enabled_log_value_count = 0;
    uint16_t enabled_log_switch_count = 0;
    uint16_t log_value_count = logValues->log_value_id.length();
    uint16_t log_switch_count = logValues->log_switch_id.length();

    received.remove(0, 5);

    // emit LOG_D(parse_message_to_hex(received), true, true);

    logValues->log_values_by_protocol.clear();

    // emit LOG_D("Parsing log values list", true, true), true, true);
    for (int i = 0; i < log_value_count; i++)
    {
        if (logValues->log_value_protocol.at(i) == protocol)
        {
            logValues->log_values_by_protocol.append(logValues->log_value_name.at(i));

            uint16_t ecu_byte_index = logValues->log_value_ecu_byte_index.at(i).toUInt();
            if (ecu_byte_index < received.length() && logValues->log_value_ecu_byte_index.at(i) != "No byte index")
            {
                // emit LOG_D("Value: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i), true, true);
                uint8_t ecu_bit = logValues->log_value_ecu_bit.at(i).toUInt();
                uint16_t value = (uint8_t)received.at(ecu_byte_index);
                if (((value) & (1 << (ecu_bit))))
                {
                    logValues->log_value_enabled.replace(i, "1");
                    emit LOG_D("Byte: 0x" + QString::number(value, 16) + " - ECU byte index " + QString::number(ecu_byte_index) + " and bit " + QString::number(ecu_bit) + " is enabled: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i) + " " + logValues->log_value_enabled.at(i), true, true);
                    enabled_log_value_count++;
                }
                else
                {
                    logValues->log_value_enabled.replace(i, "0");
                    // emit LOG_D("Disabled: " + logValues->log_value_id.at(i) + " " + logValues->log_value_name.at(i) + " " + logValues->log_value_enabled.at(i), true, true);
                }
            }
            else
                logValues->log_value_enabled[i] = "0";
        }
    }
    // emit LOG_D("Log values list ready", true, true);

    // emit LOG_D("Parsing log switches list", true, true);
    for (int i = 0; i < log_switch_count; i++)
    {
        if (logValues->log_switch_protocol.at(i) == protocol)
        {
            // 'switch_byte_index' is byte index in SSM init response
            uint16_t switch_byte_index = logValues->log_switch_ecu_byte_index.at(i).toUInt();
            if (switch_byte_index < received.length())
            {
                uint8_t switch_bit = logValues->log_switch_ecu_bit.at(i).toUInt();
                // emit LOG_D("1 " + switch_byte_index, true, true);
                uint8_t value = (uint8_t)received.at(switch_byte_index);
                // emit LOG_D("2", true, true);
                if (((value) & (1 << (switch_bit))))
                {
                    logValues->log_switch_enabled.replace(i, "1");
                    // emit LOG_D("Switch: " + logValues->log_switch_id.at(i) + " " + logValues->log_switch_name.at(i) + " " + logValues->log_switch_enabled.at(i), true, true);
                    enabled_log_switch_count++;
                }
                else
                    logValues->log_switch_enabled.replace(i, "0");
            }
        }
    }
    fileActions->read_logger_conf(logValues, ecuid, false);
    // emit LOG_D("Log switches list ready", true, true);

    update_logboxes(protocol);
}

QByteArray MainWindow::add_ssm_header(QByteArray output, bool dec_0x100)
{
    uint8_t length = output.length();

    output.insert(0, (uint8_t)0x80);
    if (ecu_radio_button->isChecked())
    {
        emit LOG_D("Set target ID to ECU ", true, true);
        output.insert(1, (uint8_t)0x10);
    }
    else
    {
        emit LOG_D("Set target ID to TCU", true, true);
        output.insert(1, (uint8_t)0x18);
    }
    output.insert(2, (uint8_t)0xF0);
    output.insert(3, length);
    output.append(calculate_checksum(output, dec_0x100));

    emit LOG_D("Generated SSM message: " + parse_message_to_hex(output), true, true);

    return output;
}

uint8_t MainWindow::calculate_checksum(QByteArray output, bool dec_0x100)
{
    uint8_t checksum = 0;

    for (uint16_t i = 0; i < output.length(); i++)
    {
        checksum += (uint8_t)output.at(i);
    }
    if (dec_0x100)
        checksum = (uint8_t)(0x100 - checksum);

    return checksum;
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
                log_file_name.append("/");
            log_file_name.append("fastecu_" + dateTimeString + ".csv");

            datalog_file.setFileName(log_file_name);
            if (!datalog_file.open(QIODevice::WriteOnly))
            {
                QMessageBox::information(this, tr("Unable to open file"),
                                         datalog_file.errorString());
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
                datalog_file_outstream << logValues->log_value_name.at(logValues->log_value_id.indexOf(logValues->dashboard_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value_name.at(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_switch_name.at(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j), 0)) << ",";
            }
            datalog_file_outstream << "\n";
        }
        else
        {

            datalog_file_outstream << QString::number(log_file_timer->elapsed() / 1000.0f) << ",";
            for (int j = 0; j < logValues->dashboard_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value.at(logValues->log_value_id.indexOf(logValues->dashboard_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_log_value_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_value.at(logValues->log_value_id.indexOf(logValues->lower_panel_log_value_id.at(j), 0)) << ",";
            }
            for (int j = 0; j < logValues->lower_panel_switch_id.count(); j++)
            {
                datalog_file_outstream << logValues->log_switch_state.at(logValues->log_switch_id.indexOf(logValues->lower_panel_switch_id.at(j), 0)) << ",";
            }
            datalog_file_outstream << "\n";
        }
    }
}

// MUT/DMA memory read/write bench utilities. Preserved here (moved from the now-deleted
// log_operations_mitsubishi.cpp, which also held the per-cycle MUT/DMA logging loop that
// Task 8 replaced with MutDmaLoggingProtocol) because they are standalone bench helpers,
// not part of the per-cycle logging path this refactor removes -- each opens its own
// short-lived FastEcuKlineTransport/MutDmaDriver rather than using the per-session
// mut_driver/mut_transport members that were removed along with the old logging loop.
bool MainWindow::mut_write_memory(quint16 addr, const QByteArray& bytes)
{
    if (addr < 0x4000 || addr > 0xBFFF) // writable RAM window guard
    {
        emit LOG_E("MUT/DMA: refusing write outside 0x4000-0xBFFF", true, true);
        return false;
    }
    FastEcuKlineTransport tr(serial);
    AlreadyInMode init(125000);
    MutDmaDriver d(tr, init);
    return d.writeMemory(addr, bytes);
}

QByteArray MainWindow::mut_read_memory(quint16 addr, int len)
{
    FastEcuKlineTransport tr(serial);
    AlreadyInMode init(125000);
    MutDmaDriver d(tr, init);

    QByteArray out;
    int off = 0;
    while (off < len)
    {
        int chunk = qMin(40, len - off);
        QVector<Channel> ch = planReadChannels(quint16(addr + off), chunk);
        if (!d.startFreeFormLog(ch, 0xA0, 0xA1))
            break;
        out.append(reassembleRead(d.pollOnce(50)));
        off += chunk;
    }
    return out;
}
