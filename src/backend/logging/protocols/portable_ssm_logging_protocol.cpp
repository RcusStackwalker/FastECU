#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
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

bytes::Bytes BuildPollRequest(const SsmReadPlan& plan)
{
    bytes::Bytes output{0xA8, 0x01};
    output.reserve(output.size() + plan.addresses.size() * 3);
    for (const auto address : plan.addresses)
    {
        bytes::AppendU24Be(output, address);
    }
    return output;
}

Result<std::string> DecodeNumeric(bytes::ByteView data, const LoggingChannel& channel)
{
    if (data.size() != channel.length || (data.size() != 1 && data.size() != 2 && data.size() != 4))
    {
        return Fail(ErrorKind::kBadResponse, "invalid SSM numeric width");
    }
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        const auto position = channel.byte_order == NumericByteOrder::kLittleEndian ? data.size() - 1 - i : i;
        value = (value << 8) | data[position];
    }
    switch (channel.storage)
    {
    case NumericStorage::kUnsignedInteger:
        return std::to_string(value);
    case NumericStorage::kSignedInteger:
    {
        const auto bits = data.size() * 8;
        const auto signed_value = (value & (std::uint32_t{1} << (bits - 1))) != 0
                                      ? static_cast<std::int64_t>(value) - (std::int64_t{1} << bits)
                                      : static_cast<std::int64_t>(value);
        return std::to_string(signed_value);
    }
    case NumericStorage::kFloat:
    {
        if (data.size() != 4)
        {
            return Fail(ErrorKind::kBadResponse, "SSM float requires four bytes");
        }
        const auto number = std::bit_cast<float>(value);
        if (!std::isfinite(number))
        {
            return Fail(ErrorKind::kBadResponse, "nonfinite SSM float");
        }
        return std::format("{}", static_cast<double>(number));
    }
    }
    return Fail(ErrorKind::kInvalidConfig, "invalid SSM numeric storage");
}
} // namespace

SsmLoggingProtocol::SsmLoggingProtocol(fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
                                       std::vector<LoggingChannel> channels, bool target_is_ecu,
                                       bool use_openport2_adapter)
    : clock_(clock), transport_(std::move(transport)), channels_(std::move(channels)),
      read_plan_(MakeSsmReadPlan(channels_)), target_is_ecu_(target_is_ecu),
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

    if (!read_plan_.has_value())
    {
        return std::unexpected(read_plan_.error());
    }
    if (auto write_result = transport_->Write(BuildSsmHeader(BuildPollRequest(*read_plan_))); !write_result.has_value())
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

    if (payload_length != read_plan_->addresses.size())
    {
        return PollData{.responded = false};
    }
    PollData data{.responded = true};
    data.samples.reserve(channels_.size());
    for (std::size_t index = 0; index < channels_.size(); ++index)
    {
        const auto& channel = channels_[index];
        const auto raw_value = DecodeNumeric(
            bytes::ByteView{*received}.subspan(kPayloadOffset + read_plan_->response_offsets[index], channel.length),
            channel);
        if (!raw_value.has_value())
        {
            return std::unexpected(raw_value.error());
        }
        data.samples.push_back({channel.id, *raw_value});
    }
    return data;
}

fastecu::Status SsmLoggingProtocol::Stop()
{
    pending_response_bytes_.clear();
    return {};
}

} // namespace fastecu::logging
