#include "src/backend/flash/ecu/m32r_ssm_exchange.h"

#include <algorithm>
#include <format>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{

Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(IKlineFlashTransport& transport,
                                                            const ICancellationToken& cancellation,
                                                            bytes::ByteView payload, bytes::Byte tester_id,
                                                            bytes::Byte target_id, std::chrono::milliseconds timeout)
{
    if (Status sent = SendM32rSsmFrame(transport, cancellation, payload, tester_id, target_id); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after write");
    }
    return ReceiveM32rSsmOnce(transport, cancellation, timeout);
}

namespace
{
const char *PeerName(M32rSsmPeer peer)
{
    return peer == M32rSsmPeer::kTcu ? "TCU" : "ECU";
}
} // namespace

Result<bytes::Bytes> ExchangeM32rSsmRequired(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                             bytes::ByteView payload, const M32rSsmEndpoint& endpoint,
                                             std::chrono::milliseconds timeout)
{
    auto response =
        ExchangeM32rSsmOptional(transport, cancellation, payload, endpoint.tester_id, endpoint.target_id, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return Fail(ErrorKind::kTimeout, std::format("no response from {}", PeerName(endpoint.peer)));
    }
    return std::move(**response);
}

Status ExpectM32rSsmPrefix(bytes::ByteView response, bytes::ByteView prefix, M32rSsmPeer peer)
{
    constexpr std::size_t kEnvelopeBytes = 4;
    if (response.size() < kEnvelopeBytes + prefix.size())
    {
        return Fail(ErrorKind::kBadResponse, "response is too short");
    }
    if (!M32rSsmPayloadHasPrefix(response, prefix))
    {
        return Fail(ErrorKind::kBadResponse, std::format("wrong response from {}", PeerName(peer)));
    }
    return {};
}

Status RequestM32rSsmPrefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                            bytes::ByteView payload, const M32rSsmEndpoint& endpoint, bytes::ByteView prefix,
                            std::chrono::milliseconds timeout)
{
    return ExchangeM32rSsmRequired(transport, cancellation, payload, endpoint, timeout)
        .and_then([&](const bytes::Bytes& response) { return ExpectM32rSsmPrefix(response, prefix, endpoint.peer); });
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
