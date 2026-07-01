#include "mainwindow.h"
#include <QRegularExpression>
#include "protocol/mitsu_colt_can_cdbg_driver.h"
#include "protocol/fastecu_can_transport.h"
#include "serial_port_actions.h"

using namespace MitsuColtCanCdbg;

// Build the Cdbg channel list from the enabled CDBG log values, in the same
// lower_panel order the SSM/MUT_DMA display paths consume them, so streamed
// values align with the display. outIndices[i] is the index j into
// logValues->log_value_* for the channel returned at position i.
static QVector<CdbgChannel> cdbg_channels_from_logvalues(FileActions::LogValuesStructure* lv,
                                                          QVector<int>& outIndices)
{
    QVector<CdbgChannel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "CDBG"
                && lv->log_value_enabled.at(j) == "1")
            {
                CdbgChannel c;
                c.pointer = lv->log_value_address.at(j).toUInt(nullptr, 16);
                c.size = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}

void MainWindow::cdbg_start_logging()
{
    serial->set_is_iso14230_connection(false);
    serial->set_add_iso14230_header(false);
    serial->set_is_can_connection(true);
    serial->set_is_iso15765_connection(false);
    serial->set_is_29_bit_id(false);
    serial->set_can_speed("500000");
    serial->set_can_destination_address(kReplyCanId);
    serial->open_serial_port();

    cdbg_transport = new cdbg::FastEcuCanTransport(serial);
    cdbg_driver = new CdbgLogDriver(*cdbg_transport);

    QVector<int> idx;
    QVector<CdbgChannel> ch = cdbg_channels_from_logvalues(logValues, idx);
    if (!cdbg_driver->startFreeFormLog(ch))
    {
        emit LOG_E("Cdbg: session/security handshake failed", true, true);
        cdbg_stop_logging();
        return;
    }

    connect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(cdbg_poll()));
    logparams_poll_timer->start(10);
}

void MainWindow::cdbg_poll()
{
    if (!cdbg_driver || !cdbg_driver->isStreaming())
        return;

    QVector<quint32> vals = cdbg_driver->pollOnce(20);
    if (vals.isEmpty())
        return;

    QVector<int> idx;
    QVector<CdbgChannel> ch = cdbg_channels_from_logvalues(logValues, idx);

    // Conversion mirrors mitsubishi_dma_poll() in log_operations_mitsubishi.cpp
    // exactly: each raw channel value is rendered as one decimal integer and
    // fed through the RomRaider-format from_byte expression for that channel.
    for (int i = 0; i < vals.size() && i < ch.size(); ++i)
    {
        int j = idx.at(i);

        QStringList conversion = logValues->log_value_units.at(j).split(",");
        QString from_byte = conversion.at(2);
        QStringList format_str_lst = conversion.at(3).split(".");
        uint8_t format = 0;
        if (format_str_lst.length() > 1)
            format = format_str_lst.at(1).count(QRegularExpression("0"));

        QString value = QString::number(vals.at(i));
        double calc_float_value = fileActions->calculate_value_from_expression(
            fileActions->parse_stringlist_from_expression_string(from_byte, value));
        QString calc_value = QString::number(calc_float_value, 'f', format);
        logValues->log_value.replace(j, calc_value);
    }
}

void MainWindow::cdbg_stop_logging()
{
    if (logparams_poll_timer)
        logparams_poll_timer->stop();
    disconnect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(cdbg_poll()));

    delete cdbg_driver;    cdbg_driver = nullptr;
    delete cdbg_transport; cdbg_transport = nullptr;
}
