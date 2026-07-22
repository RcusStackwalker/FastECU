#include "src/backend/logging/protocols/cdbg_logging_protocol.h"
#include "src/backend/logging/romraider_conversion.h"
#include "src/platform/desktop/common/serial/serial_port_actions.h"

#include <cstdint>

using namespace MitsuColtCanCdbg;

namespace
{
QVector<CdbgChannel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int>& outIndices)
{
    QVector<CdbgChannel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++)
    {
        for (int j = 0; j < lv->log_value_id.length(); j++)
        {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j) && lv->log_value_protocol.at(j) == "CDBG")
            {
                CdbgChannel c{};
                c.pointer = lv->log_value_address.at(j).toUInt(nullptr, 16);
                c.size = static_cast<bytes::Byte>(lv->log_value_length.at(j).toUInt());
                ch.append(c);
                outIndices.append(j);
            }
        }
    }
    return ch;
}
} // namespace

CdbgLoggingProtocol::CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                                         FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport)), serial_(serial), logValues_(logValues), fileActions_(fileActions), driver_(*transport_)
{
}

fastecu::Status CdbgLoggingProtocol::start()
{
    QVector<CdbgChannel> channels = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (channels.isEmpty())
    {
        return fastecu::fail(fastecu::ErrorKind::InvalidConfig, "no CDBG log parameters selected");
    }

    serial_->set_is_iso14230_connection(false);
    serial_->set_add_iso14230_header(false);
    serial_->set_is_can_connection(true);
    serial_->set_is_iso15765_connection(false);
    serial_->set_is_29_bit_id(false);
    serial_->set_can_speed("500000");
    serial_->set_can_destination_address(kReplyCanId);
    serial_->open_serial_port();

    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }

    QString driverError;
    if (!driver_.startFreeFormLog(channels, 0, 10, &driverError))
    {
        QString err = driverError.isEmpty() ? QStringLiteral("CDBG logging session failed") : driverError;
        return fastecu::fail(fastecu::ErrorKind::BadResponse, err.toStdString());
    }
    return {};
}

fastecu::Result<PollData> CdbgLoggingProtocol::poll(int timeoutMs)
{
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }
    if (!driver_.isStreaming())
    {
        return PollData{false, {}};
    }

    QVector<std::uint32_t> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty())
    {
        return PollData{false, {}};
    }

    PollData data;
    data.responded = true;
    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i)
    {
        int j = channelLogValueIndex_.at(i);
        QString value = QString::number(vals.at(i));
        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
        data.samples.append(LogSample{j, calc_value});
    }

    return data;
}

void CdbgLoggingProtocol::stop()
{
    // CdbgLogDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
