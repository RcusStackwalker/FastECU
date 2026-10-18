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

namespace fastecu::flash
{

namespace
{

// Legacy setup-PDU provenance (operation sources at 20892df): N83M 1.5M
// lines 837-852, 878-893, 1379-1394; N83M 4M lines 845-860, 886-901,
// 1390-1405; SH72531 lines 844-854, 881-891, 1378-1388; SH72543 diesel
// lines 827-843, 866-882, 1400-1413. Diesel computes the fields while its
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

constexpr std::string_view kRejectionPrefix = "Wrong response from ECU: ";

constexpr std::chrono::milliseconds kObkProbeTimeout{200};
constexpr uds::ExchangePolicy kIdentityPolicy{.read_timeout = std::chrono::milliseconds{2000}};
constexpr bytes::Byte kSessionProbe = 0x5F;
constexpr bytes::Byte kSessionControlReply = uds::kSidDiagnosticSessionControl + 0x40;
constexpr bytes::Byte kReadDataByIdentifierReply = uds::kSidReadDataByIdentifier + 0x40;

} // namespace

bytes::Bytes DensoIso15765DownloadSetupPdu(const MemoryRegion& region)
{
    return bytes::ComposeBe(uds::kSidRequestDownload, kDataFormatIdentifier, kAddressAndLengthFormat, region.start,
                            region.length);
}

bytes::Bytes DensoIso15765UploadSetupPdu(const MemoryRegion& region)
{
    return bytes::ComposeBe(uds::kSidRequestUpload, kDataFormatIdentifier, kAddressAndLengthFormat, region.start,
                            region.length);
}

Result<bytes::Bytes> ReadDensoEncryptedPage(const CanExecutorContext& ctx, std::uint32_t address)
{
    constexpr uds::ExchangePolicy kPagePolicy{.read_timeout = std::chrono::milliseconds{2000}};
    Result<bytes::Bytes> reply =
        FatalRequest(ExchangeContext(ctx, kPagePolicy), bytes::ComposeBe(uds::kSidReadMemoryChunk, address),
                     kRejectionPrefix, std::format("the flash read at 0x{:x}", address));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return DensoDecryptPage(uds::Payload(*reply));
}

Status DensoIso15765WriteChunk(const CanExecutorContext& ctx, std::uint32_t address, bytes::ByteView encrypted_chunk)
{
    constexpr uds::ExchangePolicy kWritePolicy{.read_timeout = std::chrono::milliseconds{500}};
    Result<bytes::Bytes> reply = FatalRequest(ExchangeContext(ctx, kWritePolicy),
                                              bytes::ComposeBe(uds::kSidWriteMemoryChunk, address, encrypted_chunk),
                                              kRejectionPrefix, std::format("the flash write at 0x{:x}", address));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return {};
}

// Legacy reflash_block write loops: N83M 1.5M 1205-1227, N83M 4M 1206-1242,
// SH72531 1205-1227 and SH72543 diesel 1197-1218, after write_memory encrypts
// the whole ROM once (1096, 1109, 1095, 1084). Legacy's stopRequested()
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

// Legacy read_memory after setup: page loops at N83M 1.5M 922-979,
// N83M 4M 929-991, SH72531 916-1007 and SH72543 diesel 907-1001, each with
// stopRequested() at the top; stop loops at 1027-1057, 1040-1070, 1025-1056
// and 1019-1045, all at receive_timeout; padding at 1061-1068, 1072-1078,
// 1058-1069 and 1047-1058. The SH page pacing (SH72531 1007, diesel 1001)
// precedes the next B7 write, so no read timeout subsumes it. Every family
// logs the stop response: N83M 4M's legacy logs only an empty continuation
// line there (line 1066), a log-only difference dropped on purpose.
// Legacy decrypts the whole accumulated dump in one call (1059, 1072, 1058,
// 1047); SsmProtocol::calculatePayload transforms independent 4-byte words, so
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

// Legacy close-block loops: N83M 1.5M 1255-1289, N83M 4M 1269-1303,
// SH72531 1261-1293 and SH72543 diesel 1282-1311. Each re-sends 37;
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

// Legacy settle and checksum verify: N83M 1.5M 1291-1352, N83M 4M 1305-1362,
// SH72531 1295-1347 and SH72543 diesel 1313-1365; the request bytes and the
// settle are identical in all four. The settle precedes a write, so no read
// timeout subsumes it. Legacy also required the 7F 31 78 pending answer
// before the result; UdsClient absorbs that NRC and accepts an immediate
// positive answer too. Legacy's SH-only "Checksum not verified" line after a
// rejection (SH72531 1315/1325, diesel 1333/1343) is dropped on purpose:
// FatalQuery already logs the rejection and the error propagates.
Status DensoIso15765CloseAndVerifyChecksum(const CanExecutorContext& ctx, const uds::ExchangePolicy& checksum_policy)
{
    DensoIso15765BestEffortTransferExit(ctx);
    if (const Status slept = ctx.clock.Sleep(std::chrono::milliseconds{100}, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }
    LogInfo(ctx, "Verifying checksum");
    if (Result<bytes::Bytes> checksum = FatalQuery(
            ctx,
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, kRoutineIdHigh, kRoutineChecksum, 0x01},
            bytes::Bytes{uds::kRoutineControlStart, kRoutineIdHigh}, checksum_policy, "checksum verify");
        !checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    LogInfo(ctx, "Checksum verified");
    return {};
}

Status DensoIso15765WriteImageAndVerifyChecksum(const CanExecutorContext& ctx, bytes::ByteView image,
                                                std::uint32_t image_base, const MemoryRegion& block,
                                                PhaseReporter& progress, const uds::ExchangePolicy& checksum_policy)
{
    if (const Status written = DensoIso15765WriteImage(ctx, image, image_base, block, progress); !written.has_value())
    {
        return written;
    }
    return DensoIso15765CloseAndVerifyChecksum(ctx, checksum_policy);
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

// Legacy kernel jumps and their bounded re-read loops: N83M 1.5M bench
// 770-802 / in car 625-655, N83M 4M 776-806 / 631-659, SH72531 770-804 /
// 625-655, SH72543 diesel 755-790 / 617-648. The bench discard is N83M 4M
// lines 788-791 and SH72531 782-785: read, delay(50), read again. The loop's
// `init_ready` flag is never read after the loop and every arm falls through
// to `return STATUS_SUCCESS`, so an unacknowledged jump is logged but is not
// an error -- deliberately not "fixed".
Status DensoIso15765JumpToKernel(const CanExecutorContext& ctx, bytes::Byte session, int max_tries,
                                 std::chrono::milliseconds first_read_timeout,
                                 std::chrono::milliseconds loop_read_timeout, bool discard_first_reply)
{
    LogInfo(ctx, "Jump to onboad kernel");
    if (const Status sent =
            ctx.channel.Send(bytes::Bytes{uds::kSidDiagnosticSessionControl, session}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(first_read_timeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (discard_first_reply)
    {
        if (const Status slept = ctx.clock.Sleep(std::chrono::milliseconds{50}, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
        received = ctx.channel.Receive(first_read_timeout, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
    }
    return DensoIso15765WaitForJumpAck(ctx, session, std::move(*received), max_tries, loop_read_timeout);
}

Result<bytes::Bytes> FatalQuery(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                const uds::ExchangePolicy& policy, std::string_view subject,
                                std::optional<std::size_t> min_payload_size)
{
    return FatalQuery(ExchangeContext(ctx, policy), pdu, expected_prefix, kRejectionPrefix, subject, min_payload_size);
}

Result<bytes::Bytes> TolerantProbe(const CanExecutorContext& ctx, bytes::ByteView pdu, bytes::Byte expected_service,
                                   bytes::Byte expected_subfunction, std::chrono::milliseconds timeout,
                                   std::string_view rejection_prefix, std::string_view subject)
{
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(timeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // Legacy requires received.length() > 5, i.e. at least two bytes past the
    // 4-byte envelope; anything shorter takes its "No valid response from
    // ECU" path and returns STATUS_ERROR.
    if (!received->has_value() || received->value().size() < 2)
    {
        LogError(ctx, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, std::format("no response from ECU during the {}", subject));
    }
    const bytes::Bytes& frame = **received;
    if (frame[0] != expected_service || frame[1] != expected_subfunction)
    {
        LogError(ctx, std::format("{}{}", rejection_prefix, bytes::ToHex(frame)));
    }
    return frame;
}

Result<bool> DensoObkIsActive(const CanExecutorContext& ctx, std::string_view active_log)
{
    LogInfo(ctx, "Checking if OBK is active...");
    if (const Status sent =
            ctx.channel.Send(bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, ctx.cancellation);
        !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    Result<std::optional<bytes::Bytes>> obk = ctx.channel.Receive(kObkProbeTimeout, ctx.cancellation);
    if (!obk.has_value())
    {
        return std::unexpected(obk.error());
    }
    constexpr std::array kObkActiveReply{kSessionControlReply, kSessionProbe};
    if (obk->has_value() && std::ranges::equal(**obk | std::views::take(kObkActiveReply.size()), kObkActiveReply))
    {
        LogInfo(ctx, active_log);
        return true;
    }
    LogInfo(ctx, "OBK not active, initialising ECU...");
    return false;
}

void DensoEcuIdQuery(const CanExecutorContext& ctx)
{
    LogInfo(ctx, "Requesting ECU ID");
    NonFatalQuery(ExchangeContext(ctx, kIdentityPolicy), bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt,
                  kRejectionPrefix, "ECU ID");
}

void DensoVehicleInfoQueries(const CanExecutorContext& ctx)
{
    const UdsExchangeContext exchange = ExchangeContext(ctx, kIdentityPolicy);
    LogInfo(ctx, "Requesting VIN");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidVin}, uds::kVehicleInfoPidVin,
                  kRejectionPrefix, "VIN");
    LogInfo(ctx, "Requesting CAL ID");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId},
                  uds::kVehicleInfoPidCalId, kRejectionPrefix, "CAL ID");
    LogInfo(ctx, "Requesting CVN");
    NonFatalQuery(exchange, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCvn}, uds::kVehicleInfoPidCvn,
                  kRejectionPrefix, "CVN");
}

Result<bool> DensoProgrammingBranchIsInCar(const CanExecutorContext& ctx, std::chrono::milliseconds probe_timeout)
{
    LogInfo(ctx, "Checking access method");
    if (Result<bytes::Bytes> access =
            TolerantProbe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionProbe}, kSessionControlReply,
                          0x01, probe_timeout, kRejectionPrefix, "access-method probe");
        !access.has_value())
    {
        return std::unexpected(access.error());
    }
    Result<bytes::Bytes> selector =
        TolerantProbe(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, 0x10, 0x1D}, kReadDataByIdentifierReply, 0x10,
                      probe_timeout, kRejectionPrefix, "programming-branch selector");
    if (!selector.has_value())
    {
        return std::unexpected(selector.error());
    }
    // Legacy reads raw frame byte 7 (payload byte 3) without a length check;
    // TolerantProbe only guarantees two bytes, so this guard is not redundant.
    if (selector->size() < 4)
    {
        LogError(ctx, "Wrong response from ECU: programming-branch selector reply is too short");
        return Fail(ErrorKind::kBadResponse, "programming-branch selector reply is too short");
    }
    return (*selector)[3] != 0xFF;
}

Status FireAndForget(const CanExecutorContext& ctx, ICanFlashTransport& can, std::uint32_t request_id,
                     bytes::ByteView pdu, std::chrono::milliseconds timeout)
{
    // The reply is read from `can` below, never through this channel, so the
    // response-id slot is filled with the request id and never consulted.
    CanFlashUdsChannel channel(can, request_id, request_id);
    if (const Status sent = channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> ignored = can.Read(timeout, ctx.cancellation);
    if (!ignored.has_value())
    {
        return std::unexpected(ignored.error());
    }
    return {};
}

Status DensoSecurityAccess(const CanExecutorContext& ctx)
{
    const UdsExchangeContext exchange = ExchangeContext(ctx, kSecurityAccessPolicy);

    LogInfo(ctx, "Starting seed request");
    Result<bytes::Bytes> seed_reply =
        FatalQuery(exchange, bytes::Bytes{uds::kSidSecurityAccess, kSecurityAccessRequestSeed},
                   bytes::Bytes{kSecurityAccessRequestSeed}, kRejectionPrefix, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    LogInfo(ctx, "Seed request ok");
    // The four seed bytes sit at payload offsets 1-4, once the service id is
    // stripped and behind the level echo.
    const bytes::Bytes key = DensoSeedKey(uds::Payload(*seed_reply).subspan(1, 4));

    LogInfo(ctx, "Sending seed key");
    Result<bytes::Bytes> key_reply =
        FatalQuery(exchange, bytes::ComposeBe(uds::kSidSecurityAccess, kSecurityAccessSendKey, key),
                   bytes::Bytes{kSecurityAccessSendKey}, kRejectionPrefix, "seed key");
    if (!key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    LogInfo(ctx, "Seed key ok");
    return {};
}

Status DensoIso15765Erase(const CanExecutorContext& ctx, const MemoryRegion& region)
{
    LogInfo(ctx, "Setting flash start & length");
    if (Result<bytes::Bytes> setup =
            FatalQuery(ExchangeContext(ctx, kErasePolicy), DensoIso15765DownloadSetupPdu(region),
                       bytes::Bytes{0x20, 0x01, 0x05}, kRejectionPrefix, "flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    LogInfo(ctx, "Erasing ECU ROM");
    // Sent through the channel rather than UdsClient: no reply is read here,
    // the polling loop below consumes the ECU's answer instead.
    if (const Status sent = ctx.channel.Send(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart,
                                                          kRoutineIdHigh, kRoutineErase, 0xff, 0xff, 0xff, 0xff},
                                             ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    if (const Status slept = ctx.clock.Sleep(kEraseTimeout, ctx.cancellation); !slept.has_value())
    {
        return slept;
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
    for (const InCarExchange& exchange : kN83mInCarSequence)
    {
        if (const Status sent = FireAndForget(ctx, can, exchange.id,
                                              bytes::ByteView(exchange.pdu.data(), exchange.length), kInCarTimeout);
            !sent.has_value())
        {
            return sent;
        }
    }
    return {};
}

} // namespace fastecu::flash
