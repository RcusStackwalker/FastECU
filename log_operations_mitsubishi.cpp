#include "mainwindow.h"
#include "protocol/mut_dma_freeform.h"
#include "protocol/mut_dma_memory.h"

using namespace mutdma;

// Build the free-form channel list from the enabled MUT_DMA log values, IN THE SAME
// ORDER parse_log_params() consumes them (lower_panel order, matching the SSM path in
// log_operations_ssm.cpp), so the streamed data aligns with the display.
static QVector<Channel> mut_channels_from_logvalues(FileActions::LogValuesStructure* lv)
{
    QVector<Channel> ch;
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

    QVector<Channel> ch = mut_channels_from_logvalues(logValues);
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

    QVector<Channel> ch = mut_channels_from_logvalues(logValues);

    // Re-serialize each channel's decoded value back to big-endian bytes in channel
    // order, so parse_log_params() reads them as a contiguous SSM-style byte buffer.
    QByteArray buf;
    for (int i = 0; i < vals.size() && i < ch.size(); ++i)
        for (int k = ch.at(i).len - 1; k >= 0; --k)
            buf.append(char(quint8((vals.at(i) >> (k * 8)) & 0xFF)));

    logging_request_active = false; // allow parse_log_params to refill the display
    parse_log_params(buf, "MUT_DMA");
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
