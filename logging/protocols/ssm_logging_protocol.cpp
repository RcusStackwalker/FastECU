#include "logging/protocols/ssm_logging_protocol.h"
#include "logging/romraider_conversion.h"
#include "protocol/qt_bytes.h"
#include <QElapsedTimer>

namespace {
constexpr int kStartTimeoutMs = 1000;
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

uint8_t SsmLoggingProtocol::ssmChecksum(const QByteArray &output) const
{
    uint8_t checksum = 0;
    for (int i = 0; i < output.length(); i++)
        checksum += (uint8_t)output.at(i);
    return checksum;
}

QByteArray SsmLoggingProtocol::buildSsmHeader(QByteArray output) const
{
    uint8_t length = (uint8_t)output.length();
    output.insert(0, (uint8_t)0x80);
    output.insert(1, targetIsEcu_ ? (uint8_t)0x10 : (uint8_t)0x18);
    output.insert(2, (uint8_t)0xF0);
    output.insert(3, length);
    output.append((char)ssmChecksum(output));
    return output;
}

QByteArray SsmLoggingProtocol::readFramedResponse(int timeoutMs)
{
    QByteArray received;
    QElapsedTimer clock;
    clock.start();

    if (useOpenport2Adapter_)
        return bytes::toQByteArray(transport_->read(timeoutMs));

    while (received.length() < 3 && clock.elapsed() < timeoutMs)
        received.append(bytes::toQByteArray(transport_->read(10)));

    while (received.length() >= 3
           && ((uint8_t)received.at(0) != 0x80 || (uint8_t)received.at(1) != 0xf0 || (uint8_t)received.at(2) != 0x10)
           && clock.elapsed() < timeoutMs) {
        received.remove(0, 1);
        received.append(bytes::toQByteArray(transport_->read(10)));
    }

    int remaining = timeoutMs - int(clock.elapsed());
    if (remaining > 0)
        received.append(bytes::toQByteArray(transport_->read(remaining)));

    return received;
}

bool SsmLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen()) {
        if (errorOut) *errorOut = "adapter disconnected";
        return false;
    }

    QByteArray output;
    output.append((uint8_t)0xA8);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x00);
    output.append((uint8_t)0x07);
    transport_->write(bytes::view(buildSsmHeader(output)));

    QByteArray received = readFramedResponse(kStartTimeoutMs);
    if (received.length() <= 6 || (uint8_t)received.at(4) != 0xe8) {
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

    QByteArray output;
    output.append((uint8_t)0xA8);
    output.append((uint8_t)0x01);
    bool ok = false;
    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_) {
                output.append((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 16));
                output.append((uint8_t)(logValues_->log_value_address.at(j).toUInt(&ok, 16) >> 8));
                output.append((uint8_t)logValues_->log_value_address.at(j).toUInt(&ok, 16));
            }
        }
    }
    transport_->write(bytes::view(buildSsmHeader(output)));

    QByteArray received = readFramedResponse(timeoutMs);
    if (received.length() <= 6 || (uint8_t)received.at(4) != 0xe8) {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    received.remove(0, 5);
    received.remove(received.length() - 1, 1);

    for (int i = 0; i < logValues_->lower_panel_log_value_id.length(); i++) {
        for (int j = 0; j < logValues_->log_value_id.length(); j++) {
            if (logValues_->lower_panel_log_value_id.at(i) == logValues_->log_value_id.at(j)
                && logValues_->log_value_protocol.at(j) == logValueProtocolFilter_
                && logValues_->log_value_enabled.at(j) == "1"
                && i < received.length()) {
                QString value;
                uint8_t length = logValues_->log_value_length.at(j).toUInt();
                for (uint8_t k = 0; k < length && (i + k) < (uint8_t)received.length(); k++)
                    value.append(QString::number((uint8_t)received.at(i + k)));

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
