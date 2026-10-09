#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::logging
{
namespace
{
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kStartTimeout{1000};

fastecu::Status CheckCancellation(const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.Cancelled())
    {
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "SSM logging cancelled");
    }
    return {};
}

void AppendBytes(bytes::Bytes& target, bytes::Bytes chunk)
{
    target.insert(target.end(), chunk.begin(), chunk.end());
}

bytes::Bytes BuildPollRequest(const std::vector<LoggingChannel>& channels)
{
    bytes::Bytes output{0xA8, 0x01};
    output.reserve(output.size() + channels.size() * 3);
    for (const LoggingChannel& channel : channels)
    {
        bytes::AppendU24Be(output, channel.address);
    }
    return output;
}

std::vector<std::size_t> SequentialOffsets(std::size_t count)
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

SsmLoggingProtocol::SsmLoggingProtocol(fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
                                       std::vector<LoggingChannel> channels, bool target_is_ecu,
                                       bool use_openport2_adapter)
    : clock_(clock), transport_(std::move(transport)), channels_(std::move(channels)),
      response_offsets_(SequentialOffsets(channels_.size())), target_is_ecu_(target_is_ecu),
      use_openport2_adapter_(use_openport2_adapter)
{
}

SsmLoggingProtocol::SsmLoggingProtocol(fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
                                       std::vector<LoggingChannel> channels, std::vector<std::size_t> response_offsets,
                                       bool target_is_ecu, bool use_openport2_adapter)
    : clock_(clock), transport_(std::move(transport)), channels_(std::move(channels)),
      response_offsets_(std::move(response_offsets)), target_is_ecu_(target_is_ecu),
      use_openport2_adapter_(use_openport2_adapter)
{
}

bytes::Bytes SsmLoggingProtocol::BuildSsmHeader(bytes::ByteView output) const
{
    return ssm_protocol::AddHeader(output, 0xF0, target_is_ecu_ ? 0x10 : 0x18);
}

fastecu::Result<bytes::Bytes> SsmLoggingProtocol::ReadFramedResponse(std::chrono::milliseconds timeout,
                                                                     const fastecu::ICancellationToken& cancellation)
{
    bytes::Bytes received;
    const auto deadline = clock_.Now() + timeout;

    const auto read_and_append = [&](std::chrono::milliseconds read_timeout) -> fastecu::Status
    {
        if (auto status = CheckCancellation(cancellation); !status.has_value())
        {
            return status;
        }
        auto chunk = transport_->Read(read_timeout, cancellation);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (chunk->has_value())
        {
            AppendBytes(received, std::move(chunk->value()));
        }
        return {};
    };

    if (use_openport2_adapter_)
    {
        if (auto status = read_and_append(timeout); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        return received;
    }

    received = std::exchange(pending_response_bytes_, {});
    while (true)
    {
        if (auto status = CheckCancellation(cancellation); !status.has_value())
        {
            return std::unexpected(status.error());
        }
        while (received.size() >= 3 &&
               (received[0] != 0x80 || received[1] != 0xf0 || received[2] != (target_is_ecu_ ? 0x10 : 0x18)))
        {
            received.erase(received.begin());
        }
        if (received.size() >= 4)
        {
            const auto frame_size = static_cast<std::size_t>(received[3]) + 5;
            if (received.size() >= frame_size)
            {
                // Continuous replies may arrive consecutively or in one read.
                pending_response_bytes_.assign(received.begin() + static_cast<std::ptrdiff_t>(frame_size),
                                               received.end());
                received.resize(frame_size);
                return received;
            }
        }
        // Process bytes already read, including a complete frame arriving at
        // the deadline, before deciding whether another read is permitted.
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock_.Now());
        if (remaining <= 0ms)
        {
            break;
        }
        if (auto status = read_and_append(std::min(10ms, remaining)); !status.has_value())
        {
            return std::unexpected(status.error());
        }
    }

    return received;
}

fastecu::Status SsmLoggingProtocol::Start(const fastecu::ICancellationToken& cancellation)
{
    if (auto status = CheckCancellation(cancellation); !status.has_value())
    {
        return status;
    }
    if (!transport_->IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }

    pending_response_bytes_.clear();
    const bytes::Bytes output{0xA8, 0x00, 0x00, 0x00, 0x07};
    if (auto write_result = transport_->Write(BuildSsmHeader(output)); !write_result.has_value())
    {
        return std::unexpected(write_result.error());
    }

    auto received = ReadFramedResponse(kStartTimeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // The startup probe requests exactly one address and one data byte.
    if (received->size() != 7 || received->at(0) != 0x80 || received->at(1) != 0xf0 ||
        received->at(2) != (target_is_ecu_ ? 0x10 : 0x18) || received->at(3) != 2 || received->at(4) != 0xe8 ||
        bytes::Sum8(bytes::ByteView{*received}.first(6)) != received->back())
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "no response to logging start request");
    }
    return {};
}

fastecu::Result<PollData> SsmLoggingProtocol::Poll(std::chrono::milliseconds timeout,
                                                   const fastecu::ICancellationToken& cancellation)
{
    if (auto status = CheckCancellation(cancellation); !status.has_value())
    {
        return std::unexpected(status.error());
    }
    if (!transport_->IsOpen())
    {
        return fastecu::Fail(fastecu::ErrorKind::kDisconnected, "adapter disconnected");
    }

    if (auto write_result = transport_->Write(BuildSsmHeader(BuildPollRequest(channels_))); !write_result.has_value())
    {
        return std::unexpected(write_result.error());
    }

    auto received = ReadFramedResponse(timeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 6 || received->at(0) != 0x80 || received->at(1) != 0xf0 ||
        received->at(2) != (target_is_ecu_ ? 0x10 : 0x18) ||
        received->size() != static_cast<std::size_t>(received->at(3)) + 5 || received->at(4) != 0xe8 ||
        bytes::Sum8(bytes::ByteView{*received}.first(received->size() - 1)) != received->back())
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
        for (std::size_t byte_index = 0; byte_index < channel.length && response_offset + byte_index < payload_length;
             ++byte_index)
        {
            raw_value += std::to_string(received->at(kPayloadOffset + response_offset + byte_index));
        }
        data.samples.push_back(ProtocolSample{
            .channel_id = channel.id,
            .raw_value = std::move(raw_value),
        });
    }
    return data;
}

fastecu::Status SsmLoggingProtocol::Stop()
{
    pending_response_bytes_.clear();
    return {};
}

} // namespace fastecu::logging
