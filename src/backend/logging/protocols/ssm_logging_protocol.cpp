#include "src/backend/logging/protocols/ssm_logging_protocol.h"
#include "src/backend/logging/romraider_conversion.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include <QElapsedTimer>
#include <QHash>

#include <cstddef>
#include <cstdint>

namespace
{
constexpr int kStartTimeoutMs = 1000;

struct SsmLogChannel
{
    int logValueIndex = 0;
    std::size_t responseOffset = 0;
    std::uint32_t address = 0;
    std::size_t length = 0;
    bool enabled = false;
};

void appendBytes(bytes::Bytes& target, bytes::Bytes chunk)
{
    target.insert(target.end(), chunk.begin(), chunk.end());
}

QVector<SsmLogChannel> channelsFromLogValues(FileActions::LogValuesStructure *lv,
                                             const QString& protocolFilter)
{
    QHash<QString, QVector<int>> logValueIndicesById;
    logValueIndicesById.reserve(lv->log_value_id.length());
    for (int j = 0; j < lv->log_value_id.length(); ++j)
    {
        if (lv->log_value_protocol.at(j) == protocolFilter)
        {
            logValueIndicesById[lv->log_value_id.at(j)].append(j);
        }
    }

    QVector<SsmLogChannel> channels;
    channels.reserve(lv->lower_panel_log_value_id.length());
    for (int i = 0; i < lv->lower_panel_log_value_id.length(); ++i)
    {
        const auto it = logValueIndicesById.constFind(lv->lower_panel_log_value_id.at(i));
        if (it == logValueIndicesById.cend())
        {
            continue;
        }

        for (int j : it.value())
        {
            SsmLogChannel channel;
            channel.logValueIndex = j;
            channel.responseOffset = static_cast<std::size_t>(i);
            channel.address = lv->log_value_address.at(j).toUInt(nullptr, 16);
            channel.length = lv->log_value_length.at(j).toUInt();
            channel.enabled = lv->log_value_enabled.at(j) == "1";
            channels.append(channel);
        }
    }
    return channels;
}

bytes::Bytes buildPollRequest(const QVector<SsmLogChannel>& channels)
{
    bytes::Bytes output{0xA8, 0x01};
    output.reserve(output.size() + channels.size() * 3);
    for (const SsmLogChannel& channel : channels)
    {
        bytes::appendU24Be(output, channel.address);
    }
    return output;
}
} // namespace

SsmLoggingProtocol::SsmLoggingProtocol(std::unique_ptr<ISsmTransport> transport,
                                       FileActions::LogValuesStructure *logValues, FileActions *fileActions,
                                       QString logValueProtocolFilter, bool targetIsEcu, bool useOpenport2Adapter)
    : transport_(std::move(transport)), logValues_(logValues), fileActions_(fileActions), logValueProtocolFilter_(std::move(logValueProtocolFilter)), targetIsEcu_(targetIsEcu), useOpenport2Adapter_(useOpenport2Adapter)
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
    {
        return transport_->read(timeoutMs);
    }

    while (received.size() < 3 && clock.elapsed() < timeoutMs)
    {
        appendBytes(received, transport_->read(10));
    }

    while (received.size() >= 3 && (received[0] != 0x80 || received[1] != 0xf0 || received[2] != 0x10) && clock.elapsed() < timeoutMs)
    {
        received.erase(received.begin());
        appendBytes(received, transport_->read(10));
    }

    int remaining = timeoutMs - int(clock.elapsed());
    if (remaining > 0)
    {
        appendBytes(received, transport_->read(remaining));
    }

    return received;
}

bool SsmLoggingProtocol::start(QString *errorOut)
{
    if (!transport_->isOpen())
    {
        if (errorOut)
        {
            *errorOut = "adapter disconnected";
        }
        return false;
    }

    const bytes::Bytes output{0xA8, 0x00, 0x00, 0x00, 0x07};
    transport_->write(buildSsmHeader(output));

    const bytes::Bytes received = readFramedResponse(kStartTimeoutMs);
    if (received.size() <= 6 || received[4] != 0xe8)
    {
        if (errorOut)
        {
            *errorOut = "no response to logging start request";
        }
        return false;
    }
    return true;
}

PollResult SsmLoggingProtocol::poll(int timeoutMs)
{
    PollResult result;

    if (!transport_->isOpen())
    {
        result.status = PollResult::Status::TransportError;
        result.errorMessage = "adapter disconnected";
        return result;
    }

    const QVector<SsmLogChannel> channels = channelsFromLogValues(logValues_, logValueProtocolFilter_);
    transport_->write(buildSsmHeader(buildPollRequest(channels)));

    const bytes::Bytes received = readFramedResponse(timeoutMs);
    if (received.size() <= 6 || received[4] != 0xe8)
    {
        result.status = PollResult::Status::NoResponse;
        return result;
    }

    const std::size_t payloadOffset = 5;
    const std::size_t payloadLength = received.size() - payloadOffset - 1;

    for (const SsmLogChannel& channel : channels)
    {
        const std::size_t channelOffset = channel.responseOffset;
        if (!channel.enabled || channelOffset >= payloadLength)
        {
            continue;
        }

        QString value;
        for (std::size_t k = 0; k < channel.length && channelOffset + k < payloadLength; ++k)
        {
            value.append(QString::number(received[payloadOffset + channelOffset + k]));
        }

        QString calc_value = convertRomRaiderValue(fileActions_, logValues_, channel.logValueIndex, value);
        result.samples.append(LogSample{channel.logValueIndex, calc_value});
    }

    result.status = PollResult::Status::Ok;
    return result;
}

void SsmLoggingProtocol::stop()
{
    // No explicit "stop logging" wire command in the original protocol; the ECU
    // simply stops being polled once this object is destroyed.
}
