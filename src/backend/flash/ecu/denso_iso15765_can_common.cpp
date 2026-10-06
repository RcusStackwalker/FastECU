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
constexpr uds::ExchangePolicy kErasePolicy{.read_timeout = std::chrono::milliseconds{500}};
constexpr std::chrono::milliseconds kEraseTimeout{500};
constexpr int kErasePollLimit = 20;

// Routine identifier shared by the erase (0x02 0x01) and checksum (0x02 0x02)
// routines; vendor-assigned.
constexpr bytes::Byte kRoutineIdHigh = 0x02;
constexpr bytes::Byte kRoutineErase = 0x01;
constexpr bytes::Byte kRoutineControlReply = uds::kSidRoutineControl + 0x40;

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

Status denso_iso15765_erase(const CanExecutorContext& ctx, bytes::ByteView request_download_setup_pdu)
{
    info(ctx, "Setting flash start & length");
    if (Result<bytes::Bytes> setup =
            fatal_query(exchange_context(ctx, kErasePolicy), request_download_setup_pdu, bytes::Bytes{0x20, 0x01, 0x05},
                        kRejectionPrefix, "flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    info(ctx, "Erasing ECU ROM");
    // Sent through the channel rather than UdsClient: no reply is read here,
    // the polling loop below consumes the ECU's answer instead.
    if (const Status sent = ctx.channel.send(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart,
                                                          kRoutineIdHigh, kRoutineErase, 0xff, 0xff, 0xff, 0xff},
                                             ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    if (const Status slept = ctx.clock.sleep(kEraseTimeout, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    for (int attempt = 0; attempt < kErasePollLimit; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> received = ctx.channel.receive(kEraseTimeout, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (received->has_value() && received->value().size() > 2 && (**received)[0] == kRoutineControlReply &&
            (**received)[1] == uds::kRoutineControlStart && (**received)[2] == kRoutineIdHigh)
        {
            info(ctx, "Flash erased! Starting flash write, do not power off!");
            return {};
        }
        if (const Status slept = ctx.clock.sleep(kEraseTimeout, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
    }

    error(ctx, "Flash area erase failed");
    return fail(ErrorKind::BadResponse, "flash area erase failed");
}

} // namespace fastecu::flash
