#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

#include <cstddef>
#include <string>
#include <utility>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::logging
{
namespace
{
constexpr int kStartTimeoutMs = 1000;

fastecu::Status checkCancellation(
    const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled,
                             "SSM logging cancelled");
    }
    return {};
}

void appendBytes(bytes::Bytes& target, bytes::Bytes chunk)
{
    target.insert(target.end(), chunk.begin(), chunk.end());
}

bytes::Bytes buildPollRequest(const std::vector<LoggingChannel>& channels)
{
    bytes::Bytes output{0xA8, 0x01};
    output.reserve(output.size() + channels.size() * 3);
    for (const LoggingChannel& channel : channels)
    {
        bytes::appendU24Be(output, channel.address);
    }
    return output;
}

std::vector<std::size_t> sequentialOffsets(std::size_t count)
{
    std::vector<std::size_t> offsets;
    offsets.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        offsets.push_back(index);
    }
    return offsets;
}
} // namespace

SsmLoggingProtocol::SsmLoggingProtocol(
    fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
    std::vector<LoggingChannel> channels, bool target_is_ecu,
    bool use_openport2_adapter)
    : clock_(clock),
      transport_(std::move(transport)),
      channels_(std::move(channels)),
      response_offsets_(sequentialOffsets(channels_.size())),
      target_is_ecu_(target_is_ecu),
      use_openport2_adapter_(use_openport2_adapter)
{
}

SsmLoggingProtocol::SsmLoggingProtocol(
    fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
    std::vector<LoggingChannel> channels,
    std::vector<std::size_t> response_offsets, bool target_is_ecu,
    bool use_openport2_adapter)
    : clock_(clock),
      transport_(std::move(transport)),
      channels_(std::move(channels)),
      response_offsets_(std::move(response_offsets)),
      target_is_ecu_(target_is_ecu),
      use_openport2_adapter_(use_openport2_adapter)
{
}

bytes::Bytes SsmLoggingProtocol::buildSsmHeader(bytes::ByteView output) const
{
    return SsmProtocol::addHeader(output, 0xF0, target_is_ecu_ ? 0x10 : 0x18);
}

fastecu::Result<bytes::Bytes> SsmLoggingProtocol::readFramedResponse(
    int timeout_ms, const fastecu::ICancellationToken& cancellation)
{
    bytes::Bytes received;
    const std::uint64_t start = clock_.now_ms();

    const auto read_and_append = [&](int read_timeout) -> fastecu::Status
    {
        if (auto status = checkCancellation(cancellation); !status)
        {
            return status;
        }
        auto chunk = transport_->read(read_timeout, cancellation);
        if (!chunk)
        {
            return std::unexpected(chunk.error());
        }
        if (chunk->has_value())
        {
            appendBytes(received, std::move(chunk->value()));
        }
        return {};
    };

    if (use_openport2_adapter_)
    {
        if (auto status = read_and_append(timeout_ms); !status)
        {
            return std::unexpected(status.error());
        }
        return received;
    }

    while (received.size() < 3 &&
           static_cast<int>(clock_.now_ms() - start) < timeout_ms)
    {
        if (auto status = read_and_append(10); !status)
        {
            return std::unexpected(status.error());
        }
    }

    while (received.size() >= 3 &&
           (received[0] != 0x80 || received[1] != 0xf0 || received[2] != 0x10) &&
           static_cast<int>(clock_.now_ms() - start) < timeout_ms)
    {
        received.erase(received.begin());
        if (auto status = read_and_append(10); !status)
        {
            return std::unexpected(status.error());
        }
    }

    const int remaining =
        timeout_ms - static_cast<int>(clock_.now_ms() - start);
    if (remaining > 0)
    {
        if (auto status = read_and_append(remaining); !status)
        {
            return std::unexpected(status.error());
        }
    }

    return received;
}

fastecu::Status SsmLoggingProtocol::start(
    const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return status;
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected,
                             "adapter disconnected");
    }

    const bytes::Bytes output{0xA8, 0x00, 0x00, 0x00, 0x07};
    auto write_result = transport_->write(buildSsmHeader(output));
    if (!write_result)
    {
        return std::unexpected(write_result.error());
    }

    auto received = readFramedResponse(kStartTimeoutMs, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 6 || received->at(4) != 0xe8)
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse,
                             "no response to logging start request");
    }
    return {};
}

fastecu::Result<PollData> SsmLoggingProtocol::poll(
    int timeout_ms, const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return std::unexpected(status.error());
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected,
                             "adapter disconnected");
    }

    auto write_result = transport_->write(buildSsmHeader(buildPollRequest(channels_)));
    if (!write_result)
    {
        return std::unexpected(write_result.error());
    }

    auto received = readFramedResponse(timeout_ms, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 6 || received->at(4) != 0xe8)
    {
        return PollData{.responded = false};
    }

    constexpr std::size_t kPayloadOffset = 5;
    const std::size_t payload_length = received->size() - kPayloadOffset - 1;

    PollData data{.responded = true};
    data.samples.reserve(channels_.size());
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        const LoggingChannel& channel = channels_[index];
        const std::size_t response_offset = response_offsets_.at(index);
        if (response_offset >= payload_length)
        {
            continue;
        }

        std::string raw_value;
        for (std::size_t byte_index = 0;
             byte_index < channel.length &&
             response_offset + byte_index < payload_length;
             ++byte_index)
        {
            raw_value += std::to_string(
                received->at(kPayloadOffset + response_offset + byte_index));
        }
        data.samples.push_back(ProtocolSample{
            .channel_id = channel.id,
            .raw_value = std::move(raw_value),
        });
    }
    return data;
}

fastecu::Status SsmLoggingProtocol::stop()
{
    return {};
}

} // namespace fastecu::logging
