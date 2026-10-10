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
    // The startup probe requests one address, hence one data byte.
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
    const auto expected_size = plan_.addresses.size() + 6;
    if (received->size() != expected_size || received->at(0) != 0x80 || received->at(1) != 0xf0 ||
        received->at(2) != (target_is_ecu_ ? 0x10 : 0x18) || received->at(3) != plan_.addresses.size() + 1 ||
        received->at(4) != 0xe8 ||
        bytes::Sum8(bytes::ByteView{*received}.first(received->size() - 1)) != received->back())
    {
        return Fail(ErrorKind::kBadResponse, "SSM logging response does not match complete physical read plan");
    }
    if (plan_.response_positions.size() != channels_.size())
    {
        return Fail(ErrorKind::kInvalidConfig, "SSM logical channel/read-plan count mismatch");
    }
    PollData data{.responded = true};
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        const auto& positions = plan_.response_positions[i];
        if (positions.size() != channels_[i].length)
        {
            return Fail(ErrorKind::kInvalidConfig, "SSM channel byte positions do not match width");
        }
        std::string raw_value;
        for (const auto position : positions)
        {
            if (position >= plan_.addresses.size())
            {
                return Fail(ErrorKind::kInvalidConfig, "SSM response position outside physical read plan");
            }
            raw_value += std::to_string(received->at(5 + position));
        }
        data.samples.push_back({channels_[i].id, std::move(raw_value)});
    }
    return data;
}

fastecu::Status SsmLoggingProtocol::Stop()
{
    pending_response_bytes_.clear();
    return {};
}

} // namespace fastecu::logging
