#include "src/backend/logging/protocols/portable_ssm_logging_protocol.h"

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

fastecu::Status checkCancellation(const fastecu::ICancellationToken& cancellation)
{
    if (cancellation.cancelled())
    {
        return fastecu::fail(fastecu::ErrorKind::Cancelled, "SSM logging cancelled");
    }
    return {};
}

void appendBytes(bytes::Bytes& target, bytes::Bytes chunk)
{
    target.insert(target.end(), chunk.begin(), chunk.end());
}

bytes::Bytes buildPollRequest(const SsmReadPlan& plan)
{
    bytes::Bytes output{0xA8, 0x01};
    output.reserve(output.size() + plan.addresses.size() * 3);
    for (const auto address : plan.addresses)
    {
        bytes::appendU24Be(output, address);
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

bytes::Bytes SsmLoggingProtocol::buildSsmHeader(bytes::ByteView output) const
{
    return SsmProtocol::addHeader(output, 0xF0, target_is_ecu_ ? 0x10 : 0x18);
}

fastecu::Result<bytes::Bytes> SsmLoggingProtocol::readFramedResponse(std::chrono::milliseconds timeout,
                                                                     const fastecu::ICancellationToken& cancellation)
{
    bytes::Bytes received;
    const auto deadline = clock_.now() + timeout;

    const auto read_and_append = [&](std::chrono::milliseconds read_timeout) -> fastecu::Status
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
        if (auto status = read_and_append(timeout); !status)
        {
            return std::unexpected(status.error());
        }
        return received;
    }

    while (received.size() < 3 && clock_.now() < deadline)
    {
        if (auto status = read_and_append(10ms); !status)
        {
            return std::unexpected(status.error());
        }
    }

    while (received.size() >= 3 &&
           (received[0] != 0x80 || received[1] != 0xf0 || received[2] != (target_is_ecu_ ? 0x10 : 0x18)) &&
           clock_.now() < deadline)
    {
        received.erase(received.begin());
        if (auto status = read_and_append(10ms); !status)
        {
            return std::unexpected(status.error());
        }
    }

    if (const auto remaining = deadline - clock_.now(); remaining > 0ms)
    {
        if (auto status = read_and_append(std::chrono::duration_cast<std::chrono::milliseconds>(remaining)); !status)
        {
            return std::unexpected(status.error());
        }
    }

    return received;
}

fastecu::Status SsmLoggingProtocol::start(const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return status;
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }

    const bytes::Bytes output{0xA8, 0x00, 0x00, 0x00, 0x07};
    if (auto write_result = transport_->write(buildSsmHeader(output)); !write_result)
    {
        return std::unexpected(write_result.error());
    }

    auto received = readFramedResponse(kStartTimeout, cancellation);
    if (!received)
    {
        return std::unexpected(received.error());
    }
    if (received->size() <= 6 || received->at(4) != 0xe8)
    {
        return fastecu::fail(fastecu::ErrorKind::BadResponse, "no response to logging start request");
    }
    return {};
}

fastecu::Result<PollData> SsmLoggingProtocol::poll(std::chrono::milliseconds timeout,
                                                   const fastecu::ICancellationToken& cancellation)
{
    if (auto status = checkCancellation(cancellation); !status)
    {
        return std::unexpected(status.error());
    }
    if (!transport_->isOpen())
    {
        return fastecu::fail(fastecu::ErrorKind::Disconnected, "adapter disconnected");
    }

    if (auto write_result = transport_->write(buildSsmHeader(buildPollRequest(plan_))); !write_result)
    {
        return std::unexpected(write_result.error());
    }

    auto received = readFramedResponse(timeout, cancellation);
    if (!received)
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
        bytes::sum8(bytes::ByteView{*received}.first(received->size() - 1)) != received->back())
    {
        return fail(ErrorKind::BadResponse, "SSM logging response does not match complete physical read plan");
    }
    if (plan_.response_positions.size() != channels_.size())
    {
        return fail(ErrorKind::InvalidConfig, "SSM logical channel/read-plan count mismatch");
    }
    PollData data{.responded = true};
    for (std::size_t i = 0; i < channels_.size(); ++i)
    {
        const auto& positions = plan_.response_positions[i];
        if (positions.size() != channels_[i].length)
        {
            return fail(ErrorKind::InvalidConfig, "SSM channel byte positions do not match width");
        }
        std::string raw_value;
        for (const auto position : positions)
        {
            if (position >= plan_.addresses.size())
            {
                return fail(ErrorKind::InvalidConfig, "SSM response position outside physical read plan");
            }
            raw_value += std::to_string(received->at(5 + position));
        }
        data.samples.push_back({channels_[i].id, std::move(raw_value)});
    }
    return data;
}

fastecu::Status SsmLoggingProtocol::stop()
{
    return {};
}

} // namespace fastecu::logging
