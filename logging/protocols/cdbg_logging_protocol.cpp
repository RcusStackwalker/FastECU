#include "logging/protocols/cdbg_logging_protocol.h"
#include "logging/romraider_conversion.h"
#include "serial_port/serial_port_actions.h"

using namespace MitsuColtCanCdbg;

namespace {
QVector<CdbgChannel> channelsFromLogValues(FileActions::LogValuesStructure *lv, QVector<int> &outIndices)
{
    QVector<CdbgChannel> ch;
    outIndices.clear();
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < lv->log_value_id.length(); j++) {
            if (lv->lower_panel_log_value_id.at(i) == lv->log_value_id.at(j)
                && lv->log_value_protocol.at(j) == "CDBG"
                && lv->log_value_enabled.at(j) == "1") {
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
}

CdbgLoggingProtocol::CdbgLoggingProtocol(std::unique_ptr<cdbg::ICanTransport> transport, SerialPortActions *serial,
                                          FileActions::LogValuesStructure *logValues, FileActions *fileActions)
    : transport_(std::move(transport))
    , serial_(serial)
    , logValues_(logValues)
    , fileActions_(fileActions)
    , driver_(*transport_)
{
}

bool CdbgLoggingProtocol::start(QString *errorOut)
{
    serial_->set_is_iso14230_connection(false);
    serial_->set_add_iso14230_header(false);
    serial_->set_is_can_connection(true);
    serial_->set_is_iso15765_connection(false);
    serial_->set_is_29_bit_id(false);
    serial_->set_can_speed("500000");
    serial_->set_can_destination_address(kReplyCanId);
    serial_->open_serial_port();

    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    QVector<CdbgChannel> channels = channelsFromLogValues(logValues_, channelLogValueIndex_);
    if (!driver_.startFreeFormLog(channels)) {
        if (errorOut) *errorOut = "Cdbg session/security handshake failed";
        return false;
    }
    return true;
}

PollResult CdbgLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }
    if (!driver_.isStreaming()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    QVector<quint32> vals = driver_.pollOnce(timeoutMs);
    if (vals.isEmpty()) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    for (int i = 0; i < vals.size() && i < channelLogValueIndex_.size(); ++i) {
        int j = channelLogValueIndex_.at(i);
        QString value = QString::number(vals.at(i));
        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
        result.samples.append(LogSample{j, calc_value});
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void CdbgLoggingProtocol::stop()
{
    // CdbgLogDriver has no explicit "stop streaming" wire command; the ECU's
    // stream simply stops being read once this protocol/driver is destroyed.
}
