#include "src/backend/flash/ecu/uds_client_exchange_common.h"

#include <algorithm>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/ports/logged_step.h"

namespace fastecu::flash
{

Error ReportExchangeFailure(IEventSink& events, const Error& failure, std::string_view rejection_prefix,
                            std::string_view operation)
{
    if (failure.kind == ErrorKind::kCancelled)
    {
        events.Log(LogLevel::kWarning, std::format("Cancelled by operator during {} -- this is not an ECU "
                                                   "rejection. The request may already have reached the ECU "
                                                   "and still be running there; check the unit before "
                                                   "power-cycling it.",
                                                   operation));
        return failure;
    }
    events.Log(LogLevel::kError, std::format("{}{}", rejection_prefix, failure.detail));
    return failure;
}

Result<bytes::Bytes> FatalRequest(const UdsExchangeContext& ctx, bytes::ByteView pdu, std::string_view rejection_prefix,
                                  std::string_view operation)
{
    Result<bytes::Bytes> reply = ctx.client.Request(pdu, ctx.policy, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(ReportExchangeFailure(ctx.events, reply.error(), rejection_prefix, operation));
    }
    return reply;
}

void NonFatalQuery(const UdsExchangeContext& ctx, bytes::ByteView pdu, std::optional<bytes::Byte> expected_subfunction,
                   std::string_view rejection_prefix, std::string_view label)
{
    Result<bytes::Bytes> reply = ctx.client.Request(pdu, ctx.policy, ctx.cancellation);
    if (!reply.has_value())
    {
        ctx.events.Log(LogLevel::kError, std::format("{}{}", rejection_prefix, reply.error().detail));
        return;
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (expected_subfunction.has_value() && (payload.empty() || payload[0] != *expected_subfunction))
    {
        ctx.events.Log(LogLevel::kError, std::format("{}unexpected subfunction", rejection_prefix));
        return;
    }
    ctx.events.Log(LogLevel::kInfo, std::format("{}: {}", label, bytes::ToHex(payload)));
}

Result<bytes::Bytes> FatalQuery(const UdsExchangeContext& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                std::string_view rejection_prefix, std::string_view subject,
                                std::optional<std::size_t> min_payload_size)
{
    Result<bytes::Bytes> reply = FatalRequest(ctx, pdu, rejection_prefix, std::format("the {}", subject));
    if (!reply.has_value())
    {
        return reply;
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (const std::size_t required_size = min_payload_size.value_or(expected_prefix.size());
        payload.size() < required_size || !std::equal(expected_prefix.begin(), expected_prefix.end(), payload.begin()))
    {
        ctx.events.Log(LogLevel::kError, std::format("{}unexpected {} response", rejection_prefix, subject));
        return Fail(ErrorKind::kBadResponse, std::format("{} rejected", subject));
    }
    return reply;
}

Status UnlockAndKernelJump(const UdsExchangeContext& ctx, std::string_view rejection_prefix,
                           bytes::Bytes (*seed_key)(bytes::ByteView seed))
{
    constexpr bytes::Byte kSessionKernelJump = 0x42;
    LogInfo(ctx, "Requesting seed");
    return FatalQuery(ctx, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
                      bytes::Bytes{uds::kSecurityAccessRequestSeed}, rejection_prefix, "seed request", 5)
        .and_then(
            [&](const bytes::Bytes& seed_reply)
            {
                LogInfo(ctx, "Seed request ok");
                const bytes::Bytes key = seed_key(uds::Payload(seed_reply).subspan(1, 4));
                LogInfo(ctx, "Sending seed key");
                return FatalQuery(ctx, bytes::ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, key),
                                  bytes::Bytes{uds::kSecurityAccessSendKey}, rejection_prefix, "seed key");
            })
        .and_then(LogThen(ctx.events, "Seed key ok", "Jumping to onboard kernel...",
                          [&]
                          {
                              return FatalQuery(ctx,
                                                bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionKernelJump},
                                                bytes::Bytes{kSessionKernelJump}, rejection_prefix, "kernel jump");
                          }))
        .transform([&](const bytes::Bytes&) { LogInfo(ctx, "Jump to kernel ok"); });
}

Result<bytes::Bytes> ReadEncryptedB7Pages(const UdsExchangeContext& ctx, std::string_view rejection_prefix,
                                          const MemoryRegion& region, bytes::Bytes (*decrypt_page)(bytes::ByteView),
                                          PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;
    ctx.events.Log(LogLevel::kInfo, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (std::uint32_t addr = region.start; addr < region.start + region.length; addr += kPageSize)
    {
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "read cancelled");
        }
        Result<bytes::Bytes> chunk = FatalRequest(ctx, bytes::ComposeBe(uds::kSidReadMemoryChunk, bytes::U24{addr}),
                                                  rejection_prefix, std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        rom.append_range(decrypt_page(uds::Payload(*chunk)));
        progress.Update(static_cast<int>(rom.size()));
    }
    return rom;
}

} // namespace fastecu::flash
