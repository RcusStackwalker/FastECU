#include "logging/protocols/ssm_logging_protocol.h"
#include "logging/romraider_conversion.h"
#include "modules/ssm_protocol_core.h"
#include <QElapsedTimer>

namespace {
constexpr int kStartTimeoutMs = 1000;

void appendBytes(bytes::Bytes &target, bytes::Bytes chunk)
{
    target.insert(target.end(), chunk.begin(), chunk.end());
}
}

SsmLoggingProtocol::SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport,
                                        FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                                        QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter)
    : transport_(std::move(transport))
    , logValues_(logValues)
    , fileActions_(fileActions)
    , logValueProtocolFilter_(std::move(logValueProtocolFilter))
    , targetIsEcu_(targetIsEcu)
    , useOpenport2Adapter_(useOpenport2Adapter)
{
}

bytes::Bytes SsmLoggingProtocol::buildSsmHeader(bytes::ByteView output) const
{
    return SsmProtocol::addHeader(output, 0xF0, targetIsEcu_ ? 0x10 : 0x18);
}

bytes::Bytes SsmLoggingProtocol::readFramedResponse(int timeoutMs)
{
    bytes::Bytes received;
    QElapsedTimer clock;
    clock.start();

    if (useOpenport2Adapter_)
        return transport_->read(timeoutMs);

    while (received.size() < 3 && clock.elapsed() < timeoutMs)
        appendBytes(received, transport_->read(10));

    while (received.size() >= 3
           && (received[0] != 0x80 || received[1] != 0xf0 || received[2] != 0x10)
           && clock.elapsed() < timeoutMs) {
        received.erase(received.begin());
        appendBytes(received, transport_->read(10));
    }

    int remaining = timeoutMs - int(clock.elapsed());
    if (remaining > 0)
        appendBytes(received, transport_->read(remaining));

    return received;
}

bool SsmLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    const bytes::Bytes output{0xA8, 0x00, 0x00, 0x00, 0x07};
    transport_->write(buildSsmHeader(output));

    const bytes::Bytes received = readFramedResponse(kStartTimeoutMs);
    if (received.size() <= 6 || received[4] != 0xe8) {
        if (errorOut) *errorOut = "no response to logging start request";
        return false;
    }
    return true;
}

PollResult SsmLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen()) {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }

    bytes::Bytes output{0xA8, 0x01};
    bool ok = false;
    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_) {
                output.push_back((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 16));
                output.push_back((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 8));
                output.push_back((uint8_t)logValues_->log_value_address.at(j).toUInt(&ok, 16));
            }
        }
    }
    transport_->write(buildSsmHeader(output));

    const bytes::Bytes received = readFramedResponse(timeoutMs);
    if (received.size() <= 6 || received[4] != 0xe8) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    const std::size_t payloadOffset = 5;
    const std::size_t payloadLength = received.size() - payloadOffset - 1;

    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_
                && logValues_->log_value_enabled.at(j) == "1"
                && static_cast<std::size_t>(i) < payloadLength) {
                QString value;
                uint8_t length = logValues_->log_value_length.at(j).toUInt();
                for (uint8_t k = 0; k < length && static_cast<std::size_t>(i + k) < payloadLength; k++)
                    value.append(QString::number(received[payloadOffset + static_cast<std::size_t>(i + k)]));

                QString calc_value = convertRomRaiderValue(fileActions_, logValues_, j, value);
                result.samples.append(LogSample{j, calc_value});
            }
        }
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void SsmLoggingProtocol::stop()
{
    // No explicit "stop logging" wire command in the original protocol; the ECU
    // simply stops being polled once this object is destroyed.
}
