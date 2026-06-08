#include "mainwindow.h"
#include <QRegularExpression>
#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_memory.h"

using namespace mutdma;

// Build the free-form channel list from the enabled MUT_DMA log values, IN THE SAME
// ORDER the SSM display path consumes them (lower_panel order, matching the SSM path in
// log_operations_ssm.cpp), so the streamed data aligns with the display.
//
// outIndices is filled in lockstep with the returned channels: outIndices[i] is the
// index j into logValues->log_value_* for the channel returned at position i. This lets
// the poller decode/scale/display each channel using that channel's own conversion
// (log_value_units / log_value_length) instead of relying on a contiguous byte buffer.
static QVector<Channel> mut_channels_from_logvalues(FileActions::LogValuesStructure* lv,
                                                    QVector<int>& outIndices)
{
    QVector<Channel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "MUT_DMA"
                && lv->log_value_enabled.at(j) == "1")
            {
                Channel c;
                c.id  = quint16(lv->log_value_address.at(j).toUInt(nullptr, 16));
                c.len = quint8(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}

void MainWindow::mitsubishi_dma_start_logging()
{
    mut_transport = new FastEcuKlineTransport(serial);
    // AlreadyInMode: assumes the K-Line link is already woken at 125000 baud. Swap to
    // FiveBaudInit(addrByte, baud) once the wake sequence is bench-confirmed.
    mut_init = new AlreadyInMode(125000);
    mut_driver = new MutDmaDriver(*mut_transport, *mut_init);

    QVector<int> idx;
    QVector<Channel> ch = mut_channels_from_logvalues(logValues, idx);
    if (!mut_driver->startFreeFormLog(ch, 0xA0, 0xA1))
    {
        emit LOG_E("MUT/DMA: free-form handshake failed", true, true);
        mitsubishi_dma_stop_logging();
        return;
    }

    connect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(mitsubishi_dma_poll()));
    logparams_poll_timer->start(10);
}

void MainWindow::mitsubishi_dma_poll()
{
    if (!mut_driver || !mut_driver->isStreaming())
        return;

    QVector<quint32> vals = mut_driver->pollOnce(20);
    if (vals.isEmpty())
        return;

    QVector<int> idx;
    QVector<Channel> ch = mut_channels_from_logvalues(logValues, idx);

    // Decode/scale/display each channel with a CUMULATIVE byte offset that respects the
    // channel's own length, instead of feeding a contiguous buffer to parse_log_params()
    // (which indexes by lower-panel ordinal and only works when every channel is 1 byte).
    //
    // Conversion mirrors parse_log_params() in log_operations_ssm.cpp exactly:
    //   conversion = log_value_units.at(j).split(",");
    //   from_byte  = conversion.at(2);
    //   format     = number of '0' chars after '.' in conversion.at(3)
    //   calc       = calculate_value_from_expression(parse_stringlist_from_expression_string(from_byte, value))
    //   display    = QString::number(calc, 'f', format) -> log_value.replace(j, ...)
    //
    // Raw-value representation: parse_stringlist_from_expression_string() substitutes the
    // 'x' token with the ENTIRE value string as a single numeric literal (file_actions.cpp:
    // numbers.append(x)). SSM's loop appends QString::number((uint8_t)byte) per byte, which
    // is only correct for 1-byte channels (concatenating decimal byte strings for >1 byte
    // yields garbage, e.g. {0x01,0x2C} -> "144" rather than 300). The table-data path in
    // file_actions.cpp feeds a single QString::number(value) for the same reason. So here
    // each channel's value is rendered as ONE decimal integer of its big-endian value,
    // which is what RomRaider-format from_byte expressions (x, x*0.25, ...) expect and is
    // identical to SSM for the 1-byte case.
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
        float calc_float_value = fileActions->calculate_value_from_expression(
            fileActions->parse_stringlist_from_expression_string(from_byte, value));
        QString calc_value = QString::number(calc_float_value, 'f', format);
        logValues->log_value.replace(j, calc_value);
    }
}

void MainWindow::mitsubishi_dma_stop_logging()
{
    if (logparams_poll_timer)
        logparams_poll_timer->stop();
    disconnect(logparams_poll_timer, SIGNAL(timeout()), this, SLOT(mitsubishi_dma_poll()));

    delete mut_driver;    mut_driver = nullptr;
    delete mut_transport; mut_transport = nullptr;
    delete mut_init;      mut_init = nullptr;
}

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
