#include "src/backend/flash/ecu/denso_iso15765_can_common.h"

#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"

namespace fastecu::flash
{

namespace
{

// SecurityAccess levels for this family: odd requestSeed 0x61 paired with the
// even sendKey 0x62, not the 0x01/0x02 uds_service_ids.h names.
constexpr bytes::Byte kSecurityAccessRequestSeed = 0x61;
constexpr bytes::Byte kSecurityAccessSendKey = 0x62;
constexpr uds::ExchangePolicy kSecurityAccessPolicy{.read_timeout = std::chrono::milliseconds{2000}};
constexpr std::string_view kRejectionPrefix = "Wrong response from ECU: ";

} // namespace

Result<bytes::Bytes> tolerant_probe(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                    bytes::Byte expected_subfunction, std::chrono::milliseconds timeout,
                                    std::string_view rejection_prefix, std::string_view subject)
{
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(timeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // Legacy requires received.length() > 5, i.e. at least two bytes past the
    // 4-byte envelope; anything shorter takes its "No valid response from
    // ECU" path and returns STATUS_ERROR.
    if (!received->has_value() || received->value().size() < 2)
    {
        error(ctx, "No valid response from ECU");
        return fail(ErrorKind::Timeout, std::format("no response from ECU during the {}", subject));
    }
    const bytes::Bytes& frame = **received;
    if (frame[0] != expected_service || frame[1] != expected_subfunction)
    {
        error(ctx, std::format("{}{}", rejection_prefix, bytes::toHex(frame)));
    }
    return frame;
}

Status fire_and_forget(const CanExecutorContext& ctx, ICanFlashTransport& can, std::uint32_t request_id,
                       bytes::ByteView pdu, std::chrono::milliseconds timeout)
{
    // The reply is read from `can` below, never through this channel, so the
    // response-id slot is filled with the request id and never consulted.
    CanFlashUdsChannel channel(can, request_id, request_id);
    if (const Status sent = channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> ignored = can.read(timeout, ctx.cancellation);
    if (!ignored.has_value())
    {
        return std::unexpected(ignored.error());
    }
    return {};
}

Status denso_security_access(const CanExecutorContext& ctx)
{
    const UdsExchangeContext exchange = exchange_context(ctx, kSecurityAccessPolicy);

    info(ctx, "Starting seed request");
    Result<bytes::Bytes> seed_reply =
        fatal_query(exchange, bytes::Bytes{uds::kSidSecurityAccess, kSecurityAccessRequestSeed},
                    bytes::Bytes{kSecurityAccessRequestSeed}, kRejectionPrefix, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    info(ctx, "Seed request ok");
    // The four seed bytes sit at payload offsets 1-4, once the service id is
    // stripped and behind the level echo.
    const bytes::Bytes key = denso_seed_key(uds::payload(*seed_reply).subspan(1, 4));

    info(ctx, "Sending seed key");
    Result<bytes::Bytes> key_reply =
        fatal_query(exchange, bytes::composeBe(uds::kSidSecurityAccess, kSecurityAccessSendKey, key),
                    bytes::Bytes{kSecurityAccessSendKey}, kRejectionPrefix, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    info(ctx, "Seed key ok");
    return {};
}

} // namespace fastecu::flash
