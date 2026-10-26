#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
        return fastecu::Fail(fastecu::ErrorKind::kCancelled, "logging cancelled");
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
        const auto signed_value =
            (value & (std::uint32_t{1} << (bits - 1))) != 0
                ? static_cast<std::int64_t>(value) - static_cast<std::int64_t>(std::uint64_t{1} << bits)
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

std::uint8_t TargetId(bool target_is_ecu)
{
    return target_is_ecu ? 0x10 : 0x18;
}

// Drops bytes before the first reply header from `target`. Once a whole reply
// has arrived, returns it and leaves the bytes after it in `received`.
std::optional<bytes::Bytes> TakeReply(bytes::Bytes& received, std::uint8_t target)
{
    while (received.size() >= 3 && (received[0] != 0x80 || received[1] != 0xf0 || received[2] != target))
    {
        received.erase(received.begin());
    }
    if (received.size() < 4 || received.size() < static_cast<std::size_t>(received[3]) + 5)
    {
        return std::nullopt;
    }
    const auto end = received.begin() + received[3] + 5;
    bytes::Bytes reply(received.begin(), end);
    received.erase(received.begin(), end);
    return reply;
}

// A complete read-memory (E8) reply from `target` carrying `data_size` bytes.
bool IsReadReply(bytes::ByteView reply, std::uint8_t target, std::size_t data_size)
{
    return reply.size() == data_size + 6 && reply[0] == 0x80 && reply[1] == 0xf0 && reply[2] == target &&
           reply[3] == data_size + 1 && reply[4] == 0xe8 && bytes::Sum8(reply.first(reply.size() - 1)) == reply.back();
}

// `data` holds one byte per physical plan address.
Result<std::vector<ProtocolSample>> DecodeSamples(const SsmReadPlan& plan, const std::vector<LoggingChannel>& channels,
                                                  bytes::ByteView data)
{
    if (plan.response_positions.size() != channels.size())
    {
        return Fail(ErrorKind::kInvalidConfig, "SSM logical channel/read-plan count mismatch");
    }
    std::vector<ProtocolSample> samples;
    for (std::size_t i = 0; i < channels.size(); ++i)
    {
        const auto& positions = plan.response_positions[i];
        if (positions.size() != channels[i].length)
        {
            return Fail(ErrorKind::kInvalidConfig, "SSM channel byte positions do not match width");
        }
        bytes::Bytes raw_bytes;
        for (const auto position : positions)
        {
            if (position >= data.size())
            {
                return Fail(ErrorKind::kInvalidConfig, "SSM response position outside physical read plan");
            }
            raw_bytes.push_back(data[position]);
        }
        const auto raw_value = DecodeNumeric(raw_bytes, channels[i]);
        if (!raw_value.has_value())
        {
            return std::unexpected(raw_value.error());
        }
        samples.push_back({channels[i].id, *raw_value});
    }
    return samples;
}
} // namespace

SsmLoggingProtocol::SsmLoggingProtocol(fastecu::IClock& clock, std::unique_ptr<ISsmTransport> transport,
                                       std::vector<LoggingChannel> channels, SsmReadPlan plan, bool target_is_ecu,
                                       bool use_openport2_adapter)
    : clock_(clock), transport_(std::move(transport)), channels_(std::move(channels)), plan_(std::move(plan)),
      target_is_ecu_(target_is_ecu), use_openport2_adapter_(use_openport2_adapter)
{
}

bytes::Bytes SsmLoggingProtocol::BuildSsmHeader(bytes::ByteView output) const
{
    return ssm_protocol::AddHeader(output, 0xF0, TargetId(target_is_ecu_));
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
        if (auto reply = TakeReply(received, TargetId(target_is_ecu_)); reply.has_value())
        {
            // Continuous replies may arrive consecutively or in one read.
            pending_response_bytes_ = std::move(received);
            return *std::move(reply);
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
    if (auto ready = CheckLoggingRunCanContinue(*transport_, cancellation); !ready.has_value())
    {
        return ready;
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
    // The startup probe requests one address, hence one data byte.
    if (!IsReadReply(*received, TargetId(target_is_ecu_), 1))
    {
        return fastecu::Fail(fastecu::ErrorKind::kBadResponse, "no response to logging start request");
    }
    return {};
}

fastecu::Result<PollData> SsmLoggingProtocol::Poll(std::chrono::milliseconds timeout,
                                                   const fastecu::ICancellationToken& cancellation)
{
    if (auto ready = CheckLoggingRunCanContinue(*transport_, cancellation); !ready.has_value())
    {
        return std::unexpected(ready.error());
    }

    if (auto write_result = transport_->Write(BuildSsmHeader(BuildPollRequest(plan_))); !write_result.has_value())
    {
        return std::unexpected(write_result.error());
    }

    auto received = ReadFramedResponse(timeout, cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (received->empty())
    {
        return PollData{.responded = false};
    }
    if (!IsReadReply(*received, TargetId(target_is_ecu_), plan_.addresses.size()))
    {
        return Fail(ErrorKind::kBadResponse, "SSM logging response does not match complete physical read plan");
    }
    auto samples = DecodeSamples(plan_, channels_, bytes::ByteView{*received}.subspan(5, plan_.addresses.size()));
    if (!samples.has_value())
    {
        return std::unexpected(samples.error());
    }
    return PollData{.responded = true, .samples = std::move(*samples)};
}

fastecu::Status SsmLoggingProtocol::Stop()
{
    pending_response_bytes_.clear();
    return {};
}

} // namespace fastecu::logging
