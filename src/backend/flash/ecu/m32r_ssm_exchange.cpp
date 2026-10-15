#include "src/backend/flash/ecu/m32r_ssm_exchange.h"

#include <algorithm>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{

Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(IKlineFlashTransport& transport,
                                                            const ICancellationToken& cancellation,
                                                            bytes::ByteView payload, bytes::Byte tester_id,
                                                            bytes::Byte target_id, std::chrono::milliseconds timeout)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before write");
    }
    const bytes::Bytes request = ssm_protocol::AddHeader(payload, tester_id, target_id);
    auto written = transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line write");
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after write");
    }
    auto response = transport.Read(timeout, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after read");
    }
    return response;
}

Status SendM32rSsmFrame(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                        bytes::ByteView payload, bytes::Byte tester_id, bytes::Byte target_id)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before write");
    }
    const bytes::Bytes request = ssm_protocol::AddHeader(payload, tester_id, target_id);
    auto written = transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line write");
    }
    return {};
}

Result<std::optional<bytes::Bytes>> ReceiveM32rSsmOnce(IKlineFlashTransport& transport,
                                                       const ICancellationToken& cancellation,
                                                       std::chrono::milliseconds timeout)
{
    auto response = transport.Read(timeout, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after read");
    }
    return response;
}

bool M32rSsmPayloadHasPrefix(bytes::ByteView response, bytes::ByteView prefix)
{
    constexpr std::size_t kPayloadOffset = 4;
    if (response.size() < kPayloadOffset || prefix.size() > response.size() - kPayloadOffset)
    {
        return false;
    }
    return std::ranges::equal(response.subspan(kPayloadOffset, prefix.size()), prefix);
}

} // namespace fastecu::flash
