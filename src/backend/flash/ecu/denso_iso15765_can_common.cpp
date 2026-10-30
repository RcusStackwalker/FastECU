#include "src/backend/flash/ecu/denso_iso15765_can_common.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"

namespace fastecu::flash
{

namespace
{

// Legacy setup PDUs: diesel computes the fields while its
// siblings spell out literals; all encode the supplied region identically.
constexpr bytes::Byte kDataFormatIdentifier = 0x04;
constexpr bytes::Byte kAddressAndLengthFormat = 0x44;

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
constexpr bytes::Byte kRoutineChecksum = 0x02;
constexpr bytes::Byte kRoutineControlReply = uds::kSidRoutineControl + 0x40;

constexpr std::chrono::milliseconds kInCarTimeout{200};

struct InCarExchange
{
    std::uint32_t id;
    std::array<bytes::Byte, 3> pdu;
    std::size_t length;
};

constexpr bytes::Byte kSidControlDtcSetting = 0x85;
constexpr bytes::Byte kSidCommunicationControl = 0x28;

// Write on `id`, read whatever arrives next, discard it. Order matters.
constexpr std::array<InCarExchange, 10> kN83mInCarSequence{{
    {0x7A2, {uds::kSidDiagnosticSessionControl, 0xC0, 0}, 2},
    {0x7E0, {uds::kSidDiagnosticSessionControl, 0x63, 0}, 2},
    {0x7DF, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic, 0}, 2},
    {0x7E1, {uds::kSidDiagnosticSessionControl, 0x63, 0}, 2},
    {0x7B0, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic, 0}, 2},
    {0x7B0, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7B0, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidCommunicationControl, 0x03, 0x01}, 3},
}};

// The SH72531 and SH72543 diesel run: the N83M run with session 0x03, not
// 0x63, on 0x7E1 (SH72531 and diesel legacy).
constexpr std::array<InCarExchange, 10> kShInCarSequence{{
    {0x7A2, {uds::kSidDiagnosticSessionControl, 0xC0, 0}, 2},
    {0x7E0, {uds::kSidDiagnosticSessionControl, 0x63, 0}, 2},
    {0x7DF, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic, 0}, 2},
    {0x7E1, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic, 0}, 2},
    {0x7B0, {uds::kSidDiagnosticSessionControl, uds::kSessionExtendedDiagnostic, 0}, 2},
    {0x7B0, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7B0, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidControlDtcSetting, 0x02, 0}, 2},
    {0x7DF, {kSidCommunicationControl, 0x03, 0x01}, 3},
}};

// Sends `pdu` to `request_id` and reads one reply only to discard it: the
// in-car arm's opening run of session/DTC/communication-control exchanges
// whose answers legacy never inspects. The reply is read from `can` directly,
// so the channel's response id is never consulted.
Status FireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can, std::uint32_t request_id,
                     bytes::ByteView pdu, std::chrono::milliseconds timeout)
{
    // The reply is read from `can` below, never through this channel, so the
    // response-id slot is filled with the request id and never consulted.
    CanFlashUdsChannel channel(can, request_id, request_id);
    return channel.Send(pdu, ctx.cancellation)
        .and_then([&] { return can.Read(timeout, ctx.cancellation); })
        .transform([](const std::optional<bytes::Bytes>&) {});
}

// Every reply is read at 200 ms except the primary ECU's (0x7E0) session-open
// reply, which reads at `primary_timeout`.
Status RunInCarSequence(const CanExecutorContext& ctx, ICanFlashTransport& can,
                        const std::array<InCarExchange, 10>& sequence, std::chrono::milliseconds primary_timeout)
{
    for (const InCarExchange& exchange : sequence)
    {
        if (const Status sent =
                FireAndForget(ctx, can, exchange.id, bytes::ByteView(exchange.pdu.data(), exchange.length),
                              exchange.id == 0x7E0 ? primary_timeout : kInCarTimeout);
            !sent.has_value())
        {
            return sent;
        }
    }
    return {};
}

constexpr std::chrono::milliseconds kObkProbeTimeout{200};
constexpr uds::ExchangePolicy kIdentityPolicy{.read_timeout = std::chrono::milliseconds{2000}};
constexpr bytes::Byte kSessionProbe = 0x5F;
constexpr bytes::Byte kSessionInCarOpen = 0x63;
constexpr bytes::Byte kSessionInCarJump = 0x62;
constexpr bytes::Byte kSessionBench = 0x43;
constexpr bytes::Byte kSessionBenchJump = 0x42;
constexpr bytes::Byte kSessionControlReply = uds::kSidDiagnosticSessionControl + 0x40;
constexpr bytes::Byte kReadDataByIdentifierReply = uds::kSidReadDataByIdentifier + 0x40;

} // namespace

std::array<bytes::Byte, 11> DensoIso15765DownloadSetupPdu(const MemoryRegion& region)
{
    return bytes::ComposeBe(uds::kSidRequestDownload, kDataFormatIdentifier, kAddressAndLengthFormat, region.start,
                            region.length);
}

std::array<bytes::Byte, 11> DensoIso15765UploadSetupPdu(const MemoryRegion& region)
{
    return bytes::ComposeBe(uds::kSidRequestUpload, kDataFormatIdentifier, kAddressAndLengthFormat, region.start,
                            region.length);
}

Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx, std::uint32_t address)
{
    constexpr uds::ExchangePolicy kPagePolicy{.read_timeout = std::chrono::milliseconds{2000}};
    Result<bytes::Bytes> reply =
        FatalRequest(ExchangeContext(ctx, kPagePolicy), bytes::ComposeBe(uds::kSidReadMemoryChunk, address),
                     kDensoIso15765RejectionPrefix, std::format("the flash read at 0x{:x}", address));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return DensoDecryptPage(uds::Payload(*reply));
}

Status DensoIso15765WriteChunk(const CanExecutorContext& ctx, std::uint32_t address, bytes::ByteView encrypted_chunk)
{
    constexpr uds::ExchangePolicy kWritePolicy{.read_timeout = std::chrono::milliseconds{500}};
    Result<bytes::Bytes> reply = FatalRequest(
        ExchangeContext(ctx, kWritePolicy), bytes::ComposeBe(uds::kSidWriteMemoryChunk, address, encrypted_chunk),
        kDensoIso15765RejectionPrefix, std::format("the flash write at 0x{:x}", address));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return {};
}

// Legacy reflash_block write loops run after write_memory encrypts the
// whole ROM once. Legacy's stopRequested()
// returns 0 -- success -- to write_memory, which reports a completed block;
// a cancellation mid-write must never read as a successful ROM write, so it
// fails here instead, as subaru_hitachi_m32r_can_executor.cpp does. Each
// family's index composes to the wire address minus its image base; the
// SH families' legacy pins the address's top byte to 0x00, which matches the
// big-endian encoding for every address in their regions.
Status DensoIso15765WriteImage(const CanExecutorContext& ctx, bytes::ByteView image, std::uint32_t image_base,
                               const MemoryRegion& block, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;
    const std::uint32_t max_chunks = block.length / kChunkSize;
    const bytes::Bytes encrypted = DensoEncryptRom(image);
    for (std::uint32_t chunk_index = 0; chunk_index < max_chunks; ++chunk_index)
    {
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "write cancelled");
        }
        const std::uint32_t block_addr = block.start + chunk_index * kChunkSize;
        const std::size_t image_offset = block_addr - image_base;
        if (const Status written =
                DensoIso15765WriteChunk(ctx, block_addr, bytes::ByteView(encrypted).subspan(image_offset, kChunkSize));
            !written.has_value())
        {
            return std::unexpected(written.error());
        }
        progress.Update(static_cast<int>((chunk_index + 1) * kChunkSize));
    }
    return {};
}

// Legacy read_memory after setup: page loops with stopRequested() at the
// top, then stop loops at receive_timeout, then padding. The SH page pacing
// precedes the next B7 write, so no read timeout subsumes it. Every family
// logs the stop response: N83M 4M's legacy logs only an empty continuation
// line there, a log-only difference dropped on purpose.
// Legacy decrypts the whole accumulated dump in one call;
// SsmProtocol::calculatePayload transforms independent 4-byte words, so
// decrypting each page as it arrives is byte-identical without a second
// full-ROM buffer.
Result<bytes::Bytes> DensoIso15765ReadDump(const CanExecutorContext& ctx, const MemoryRegion& region,
                                           std::size_t lead_pad_len, std::size_t tail_pad_len, PhaseReporter& progress,
                                           bool pace_pages)
{
    constexpr std::uint32_t kPageSize = 0x100;
    LogInfo(ctx, "Start reading ROM, please wait...");
    bytes::Bytes image(lead_pad_len, 0xFF);
    image.reserve(lead_pad_len + region.length + tail_pad_len);
    for (std::uint32_t offset = 0; offset < region.length; offset += kPageSize)
    {
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "read cancelled");
        }
        Result<bytes::Bytes> page = ReadDensoEncryptedPage(ctx, region.start + offset);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        image.insert(image.end(), page->begin(), page->end());
        progress.Update(static_cast<int>(offset + kPageSize));
        if (pace_pages)
        {
            if (const Status slept = ctx.clock.Sleep(std::chrono::milliseconds{1}, ctx.cancellation);
                !slept.has_value())
            {
                return std::unexpected(slept.error());
            }
        }
    }
    LogInfo(ctx, "ROM read complete");

    constexpr uds::ExchangePolicy kStopPolicy{.read_timeout = std::chrono::milliseconds{500}};
    LogInfo(ctx, "Sending stop command");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> stop =
                ctx.uds.Request(bytes::Bytes{uds::kSidRequestTransferExit}, kStopPolicy, ctx.cancellation);
            stop.has_value())
        {
            LogInfo(ctx, std::format("Stop request response: {}", bytes::ToHex(*stop)));
            break;
        }
    }

    image.insert(image.end(), tail_pad_len, 0xFF);
    return image;
}

// Legacy close-block loops each re-send 37;
// failures, including returned cancellation, are deliberately ignored.
// Success hex remains the envelope-stripped PDU used by the existing port.
void DensoIso15765BestEffortTransferExit(const CanExecutorContext& ctx)
{
    constexpr uds::ExchangePolicy kClosePolicy{.read_timeout = std::chrono::milliseconds{200}};
    ctx.events.Log(LogLevel::kInfo, "Closing out Flashing of this block");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> closed =
                ctx.uds.Request(bytes::Bytes{uds::kSidRequestTransferExit}, kClosePolicy, ctx.cancellation);
            closed.has_value())
        {
            ctx.events.Log(LogLevel::kInfo, std::format("Closed succesfully: {}", bytes::ToHex(*closed)));
            break;
        }
    }
}

// Legacy settle and checksum verify: the request bytes and the settle are
// identical in all four families. The settle precedes a write, so no read
// timeout subsumes it. Legacy also required the 7F 31 78 pending answer
// before the result; UdsClient absorbs that NRC and accepts an immediate
// positive answer too. Legacy's SH-only "Checksum not verified" line after a
// rejection is dropped on purpose:
// FatalQuery already logs the rejection and the error propagates.
Status DensoIso15765CloseAndVerifyChecksum(const CanExecutorContext& ctx, const uds::ExchangePolicy& checksum_policy)
{
    DensoIso15765BestEffortTransferExit(ctx);
    return ctx.clock.Sleep(std::chrono::milliseconds{100}, ctx.cancellation)
        .and_then(
            [&]
            {
                LogInfo(ctx, "Verifying checksum");
                return FatalQuery(ctx,
                                  bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, kRoutineIdHigh,
                                               kRoutineChecksum, 0x01},
                                  bytes::Bytes{uds::kRoutineControlStart, kRoutineIdHigh}, checksum_policy,
                                  "checksum verify");
            })
        .transform([&](const bytes::Bytes&) { LogInfo(ctx, "Checksum verified"); });
}

Status DensoIso15765WriteImageAndVerifyChecksum(const CanExecutorContext& ctx, bytes::ByteView image,
                                                std::uint32_t image_base, const MemoryRegion& block,
                                                PhaseReporter& progress, const uds::ExchangePolicy& checksum_policy)
{
    return DensoIso15765WriteImage(ctx, image, image_base, block, progress)
        .and_then([&] { return DensoIso15765CloseAndVerifyChecksum(ctx, checksum_policy); });
}

Status DensoIso15765WaitForJumpAck(const CanExecutorContext& ctx, bytes::Byte session,
                                   std::optional<bytes::Bytes> first_reply, int attempt_count,
                                   std::chrono::milliseconds receive_timeout)
{
    for (int attempt = 0; attempt < attempt_count; ++attempt)
    {
        if (first_reply.has_value() && first_reply->size() > 1 &&
            (*first_reply)[0] == uds::kSidDiagnosticSessionControl + 0x40 && (*first_reply)[1] == session)
        {
            LogInfo(ctx, "Kernel jump acknowledged");
            return {};
        }
        if (const Status slept = ctx.clock.Sleep(std::chrono::milliseconds{100}, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
        auto received = ctx.channel.Receive(receive_timeout, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        first_reply = std::move(*received);
    }
    LogError(ctx, "Kernel jump was not acknowledged; continuing, as legacy does");
    return {};
}

// Legacy kernel jumps and their bounded re-read loops. The N83M 4M and
// SH72531 bench discard is: read, delay(50), read again. The loop's
// `init_ready` flag is never read after the loop and every arm falls through
// to `return STATUS_SUCCESS`, so an unacknowledged jump is logged but is not
// an error -- deliberately not "fixed".
Status DensoIso15765JumpToKernel(const CanExecutorContext& ctx, bytes::Byte session, int max_tries,
                                 std::chrono::milliseconds first_read_timeout,
                                 std::chrono::milliseconds loop_read_timeout, bool discard_first_reply)
{
    LogInfo(ctx, "Jump to onboad kernel");
    return ctx.channel.Send(bytes::Bytes{uds::kSidDiagnosticSessionControl, session}, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(first_read_timeout, ctx.cancellation); })
        .and_then(
            [&](std::optional<bytes::Bytes> first) -> Result<std::optional<bytes::Bytes>>
            {
                if (!discard_first_reply)
                {
                    return first;
                }
                return ctx.clock.Sleep(std::chrono::milliseconds{50}, ctx.cancellation)
                    .and_then([&] { return ctx.channel.Receive(first_read_timeout, ctx.cancellation); });
            })
        .and_then(
            [&](std::optional<bytes::Bytes> reply)
            { return DensoIso15765WaitForJumpAck(ctx, session, std::move(reply), max_tries, loop_read_timeout); });
}

Result<bytes::Bytes> FatalQuery(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                const uds::ExchangePolicy& policy, std::string_view subject,
                                std::optional<std::size_t> min_payload_size)
{
    return FatalQuery(ExchangeContext(ctx, policy), pdu, expected_prefix, kDensoIso15765RejectionPrefix, subject,
                      min_payload_size);
}

namespace
{

// Sends `pdu`, reads one reply, and compares its first two bytes. A mismatch
// is LOGGED AND TOLERATED -- the frame is still returned and the caller
// decides -- which is what legacy does at these sites; only an absent or
// too-short reply is fatal. Callers that read further into the returned frame
// must length-check it themselves: this only guarantees two bytes.
//
// `timeout` is a parameter, not a constant: three families probe with their
// short timeout and subaru_denso_sh72543_can_diesel probes with its long one. Collapsing that difference is exactly
// what this file's header comment warns against.
//
Result<bytes::Bytes> TolerantProbe(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                   bytes::Byte expected_subfunction, std::chrono::milliseconds timeout,
                                   std::string_view subject)
{
    return ctx.channel.Send(pdu, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(timeout, ctx.cancellation); })
        .and_then(
            [&](std::optional<bytes::Bytes> received) -> Result<bytes::Bytes>
            {
                // Legacy requires received.length() > 5, i.e. at least two bytes past the
                // 4-byte envelope; anything shorter takes its "No valid response from
                // ECU" path and returns STATUS_ERROR.
                if (!received.has_value() || received->size() < 2)
                {
                    LogError(ctx, "No valid response from ECU");
                    return Fail(ErrorKind::kTimeout, std::format("no response from ECU during the {}", subject));
                }
                const bytes::Bytes& frame = *received;
                if (frame[0] != expected_service || frame[1] != expected_subfunction)
                {
                    LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, bytes::ToHex(frame)));
                }
                return std::move(*received);
            });
}

} // namespace

Result<bool> DensoObkIsActive(const CanExecutorContext& ctx, std::string_view active_log)
{
    LogInfo(ctx, "Checking if OBK is active...");
    return ctx.channel.Send(bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(kObkProbeTimeout, ctx.cancellation); })
        .transform(
            [&](const std::optional<bytes::Bytes>& obk)
            {
                constexpr std::array kObkActiveReply{kSessionControlReply, kSessionProbe};
                if (obk.has_value() &&
                    std::ranges::equal(*obk | std::views::take(kObkActiveReply.size()), kObkActiveReply))
                {
                    LogInfo(ctx, active_log);
                    return true;
                }
                LogInfo(ctx, "OBK not active, initialising ECU...");
                return false;
            });
}

void DensoEcuIdQuery(const CanExecutorContext& ctx)
{
    LogInfo(ctx, "Requesting ECU ID");
    NonFatalQuery(ExchangeContext(ctx, kIdentityPolicy), bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt,
                  kDensoIso15765RejectionPrefix, "ECU ID");
}

void DensoVehicleInfoQueries(const CanExecutorContext& ctx)
{
    const UdsExchangeContext exchange = ExchangeContext(ctx, kIdentityPolicy);
    LogInfo(ctx, "Requesting VIN");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidVin}, uds::kVehicleInfoPidVin,
                  kDensoIso15765RejectionPrefix, "VIN");
    LogInfo(ctx, "Requesting CAL ID");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                  uds::kVehicleInfoPidCalId, kDensoIso15765RejectionPrefix, "CAL ID");
    LogInfo(ctx, "Requesting CVN");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCvn}, uds::kVehicleInfoPidCvn,
                  kDensoIso15765RejectionPrefix, "CVN");
}

Result<bool> DensoProgrammingBranchIsInCar(const CanExecutorContext& ctx, std::chrono::milliseconds probe_timeout)
{
    LogInfo(ctx, "Checking access method");
    return TolerantProbe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, kSessionControlReply,
                         0x01, probe_timeout, "access-method probe")
        .and_then(
            [&](const bytes::Bytes&)
            {
                return TolerantProbe(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D},
                                     kReadDataByIdentifierReply, 0x10, probe_timeout, "programming-branch selector");
            })
        .and_then(
            [&](const bytes::Bytes& selector) -> Result<bool>
            {
                // Legacy reads raw frame byte 7 (payload byte 3) without a length check;
                // TolerantProbe only guarantees two bytes, so this guard is not redundant.
                if (selector.size() < 4)
                {
                    LogError(ctx, "Wrong response from ECU: programming-branch selector reply is too short");
                    return Fail(ErrorKind::kBadResponse, "programming-branch selector reply is too short");
                }
                return selector[3] != 0xFF;
            });
}

Status DensoSecurityAccess(const CanExecutorContext& ctx)
{
    const UdsExchangeContext exchange = ExchangeContext(ctx, kSecurityAccessPolicy);

    LogInfo(ctx, "Starting seed request");
    return FatalQuery(exchange, bytes::Bytes{uds::kSidSecurityAccess, kSecurityAccessRequestSeed},
                      bytes::Bytes{kSecurityAccessRequestSeed}, kDensoIso15765RejectionPrefix, "seed request", 5)
        .and_then(
            [&](const bytes::Bytes& seed_reply)
            {
                LogInfo(ctx, "Seed request ok");
                // The four seed bytes sit at payload offsets 1-4, once the service id is
                // stripped and behind the level echo.
                const bytes::Bytes key = DensoSeedKey(uds::Payload(seed_reply).subspan(1, 4));

                LogInfo(ctx, "Sending seed key");
                return FatalQuery(exchange, bytes::ComposeBe(uds::kSidSecurityAccess, kSecurityAccessSendKey, key),
                                  bytes::Bytes{kSecurityAccessSendKey}, kDensoIso15765RejectionPrefix, "seed key");
            })
        .transform([&](const bytes::Bytes&) { LogInfo(ctx, "Seed key ok"); });
}

Status DensoIso15765Erase(const CanExecutorContext& ctx, const MemoryRegion& region)
{
    LogInfo(ctx, "Setting flash start & length");
    if (const Status triggered =
            FatalQuery(ExchangeContext(ctx, kErasePolicy), DensoIso15765DownloadSetupPdu(region),
                       bytes::Bytes{0x20, 0x01, 0x05}, kDensoIso15765RejectionPrefix, "flash start & length setup")
                .and_then(
                    [&](const bytes::Bytes&)
                    {
                        LogInfo(ctx, "Erasing ECU ROM");
                        // Sent through the channel rather than UdsClient: no reply is read here,
                        // the polling loop below consumes the ECU's answer instead.
                        return ctx.channel.Send(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart,
                                                             kRoutineIdHigh, kRoutineErase, 0xff, 0xff, 0xff, 0xff},
                                                ctx.cancellation);
                    })
                .and_then([&] { return ctx.clock.Sleep(kEraseTimeout, ctx.cancellation); });
        !triggered.has_value())
    {
        return triggered;
    }

    for (int attempt = 0; attempt < kErasePollLimit; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(kEraseTimeout, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (received->has_value() && received->value().size() > 2 && (**received)[0] == kRoutineControlReply &&
            (**received)[1] == uds::kRoutineControlStart && (**received)[2] == kRoutineIdHigh)
        {
            LogInfo(ctx, "Flash erased! Starting flash write, do not power off!");
            return {};
        }
        if (const Status slept = ctx.clock.Sleep(kEraseTimeout, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
    }

    LogError(ctx, "Flash area erase failed");
    return Fail(ErrorKind::kBadResponse, "flash area erase failed");
}

Status N83mInCarFireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can)
{
    return RunInCarSequence(ctx, can, kN83mInCarSequence, kInCarTimeout);
}

// SH72531 legacy reads every reply at serial_read_short_timeout; the diesel
// reads the 0x7E0 reply at serial_read_timeout.
Status ShInCarFireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can,
                            std::chrono::milliseconds primary_timeout)
{
    return RunInCarSequence(ctx, can, kShInCarSequence, primary_timeout);
}

// Legacy in-car arms: the probe logs a mismatch and aborts only on an
// absent or short reply; unlike it, the session confirmation is fatal on a
// mismatch and expects 0x63 rather than 0x01; the selector re-read is fatal
// and its content past 62 10 is not inspected.
Status DensoIso15765ConnectInCar(const CanExecutorContext& ctx, ICanFlashTransport& can, DensoInCarRun run,
                                 std::chrono::milliseconds read_timeout)
{
    LogInfo(ctx, "In car programming: accessing, please wait...");
    const uds::ExchangePolicy policy{.read_timeout = read_timeout};
    return ctx.clock.Sleep(std::chrono::milliseconds{500}, ctx.cancellation)
        .and_then(
            [&]
            {
                return TolerantProbe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                                     kSessionControlReply, 0x01, read_timeout, "in-car access-method probe");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                return run == DensoInCarRun::kSh ? ShInCarFireAndForget(ctx, can, read_timeout)
                                                 : N83mInCarFireAndForget(ctx, can);
            })
        .and_then([&] { return DensoSecurityAccess(ctx); })
        .and_then(
            [&]
            {
                return FatalQuery(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe},
                                  bytes::Bytes{kSessionInCarOpen}, policy, "in-car session confirmation");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                return FatalQuery(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D}, bytes::Bytes{0x10},
                                  policy, "in-car programming-mode confirmation");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                return DensoIso15765JumpToKernel(ctx, kSessionInCarJump, 10, read_timeout, read_timeout,
                                                 /*discard_first_reply=*/false);
            });
}

Status DensoIso15765ConnectBench(const CanExecutorContext& ctx, const DensoBenchArm& arm)
{
    LogInfo(ctx, "Bench programming: accessing, please wait...");
    const bytes::Bytes session_pdu{uds::kSidDiagnosticSessionControl, kSessionBench};
    return ctx.clock.Sleep(arm.settle, ctx.cancellation)
        .and_then(
            [&]
            {
                return arm.session_reply == DensoBenchSessionReply::kTolerantProbe
                           ? TolerantProbe(ctx, session_pdu, kSessionControlReply, kSessionBench, arm.session_timeout,
                                           "bench diagnostic session")
                           : FatalQuery(ctx, session_pdu, bytes::Bytes{kSessionBench},
                                        {.read_timeout = arm.session_timeout}, "bench diagnostic session");
            })
        .and_then([&](const bytes::Bytes&) { return DensoSecurityAccess(ctx); })
        .and_then(
            [&]
            {
                return DensoIso15765JumpToKernel(ctx, kSessionBenchJump, 50, arm.jump_first_read, arm.jump_loop_read,
                                                 arm.discard_first_reply);
            });
}

} // namespace fastecu::flash
