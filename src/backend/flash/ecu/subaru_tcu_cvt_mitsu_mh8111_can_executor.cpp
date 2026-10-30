#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_executor.h"

#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/bootloader_operation.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/cvt_b6_chunk_exchange.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"
#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_mh8111_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/tcu/flash_tcu_cvt_subaru_mitsu_mh8111_can_operation.cpp
// it was transcribed from. Unlike SubaruTcuCvtHitachiM32rCanExecutor (Task
// 3), every one of this family's exchanges (identity queries, session,
// seed, jump, alive check) is sent on this family's OWN 0x7e1 envelope --
// there is no second "other id" CanFlashUdsChannel/UdsClient pair here.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using namespace std::chrono_literals;
using bytes::ComposeBe;
using bytes::U24;

constexpr uds::ExchangePolicy kExchangePolicy{.read_timeout = 2000ms};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific
// band -- no standard meaning, unlike uds::kSessionProgramming/
// kSessionExtendedDiagnostic.
constexpr bytes::Byte kSessionBootload = 0x43;

// Seed key (legacy generate_seed_key). Byte-identical to the sibling MH8104
// family's table (Task 6 comparison); factored into the shared
// subaru_tcu_cvt_mitsu_can_common.h/.cpp. Encrypt (write payload, legacy
// encrypt_payload) and decrypt (read payload, legacy decrypt_payload) tables:
// same Task 6 finding, same shared header.

// Hardcoded in legacy read_mem regardless of caller arguments ("hack for
// testing").
constexpr MemoryRegion kReadRegion{0x8000, 0x78000};
// The sole flashed block (block_modified skips 0-2, flashes only block 3 --
// kFlashBlocksMH8111[3] in kernelmemorymodels.h). Deliberately does NOT overlap
// kReadRegion -- see subaru_tcu_cvt_mitsu_mh8111_can_plan.cpp's comment.
constexpr MemoryRegion kWriteRegion{0x80000, 0x100000};

// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

constexpr std::string_view kRejectionPrefix = "Wrong response from TCU: ";

UdsExchangeContext ExchangeContext(Ctx& ctx)
{
    return UdsExchangeContext{ctx.uds, kExchangePolicy, ctx.cancellation, ctx.events};
}

// Thin wrapper over the shared ReportExchangeFailure (uds_client_exchange_
// common.h) for erase_memory's own hand-rolled retry loop below, which
// cannot go through FatalRequest -- it only propagates on Cancelled, not
// on every failure.
Error ReportExchangeFailure(Ctx& ctx, const Error& failure, std::string_view rejection_prefix,
                            std::string_view operation)
{
    return ::fastecu::flash::ReportExchangeFailure(ctx.events, failure, rejection_prefix, operation);
}

// Legacy's two non-fatal identity queries (TCU ID/CAL ID) and the non-fatal
// session request: logged on mismatch or absence but never halt
// connect_bootloader -- even a genuine exchange failure is logged and
// swallowed, mirroring legacy's total absence of an early return in this block.
void NonFatalQuery(Ctx& ctx, bytes::ByteView pdu, std::optional<bytes::Byte> expected_subfunction,
                   std::string_view label)
{
    ::fastecu::flash::NonFatalQuery(ExchangeContext(ctx), pdu, expected_subfunction, kRejectionPrefix, label);
}

// A raw channel exchange whose reply must open with `expected`, for steps
// whose reply service does not match the sent SID (so ctx.uds would reject
// it). Legacy logs the reply, or its absence, before failing.
Status RawExpect(Ctx& ctx, bytes::ByteView request, bytes::ByteView expected, std::string_view failure)
{
    return ctx.channel.Send(request, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(2000ms, ctx.cancellation); })
        .and_then(
            [&](const std::optional<bytes::Bytes>& reply) -> Status
            {
                if (!reply.has_value())
                {
                    LogError(ctx, "No valid response from ECU");
                    return Fail(ErrorKind::kBadResponse, std::string(failure));
                }
                if (reply->size() < expected.size() ||
                    !std::ranges::equal(bytes::ByteView(*reply).first(expected.size()), expected))
                {
                    LogError(ctx, std::format("Wrong response from TCU: {}", bytes::ToHex(*reply)));
                    return Fail(ErrorKind::kBadResponse, std::string(failure));
                }
                return {};
            });
}

// True once one of `attempts` tries of `request` succeeds.
bool RequestWithRetries(Ctx& ctx, bytes::ByteView request, int attempts)
{
    for (int attempt = 0; attempt < attempts; ++attempt)
    {
        if (ctx.uds.Request(request, kExchangePolicy, ctx.cancellation).has_value())
        {
            return true;
        }
    }
    return false;
}

// Unlike SubaruTcuCvtHitachiM32rCan (Task 3), this family has NO kernel-alive
// pre-check shortcut -- the full 7-exchange sequence below runs every time
// (Test 2, ConnectFullSequenceEveryTime, pins this).
Status ConnectBootloader(Ctx& ctx)
{
    // TCU ID 0xAA, non-fatal.
    LogInfo(ctx, "Requesting TCU ID");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt, "TCU ID");

    // CAL ID 0x09/0x04, non-fatal.
    LogInfo(ctx, "Requesting CAL ID...");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, uds::kVehicleInfoPidCalId,
                  "CAL ID");

    // Session 0x10/0x43, non-fatal -- the match branch is empty in legacy; only
    // the mismatch branch logs.
    LogInfo(ctx, "Initializing bootloader...");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBootload}, kSessionBootload,
                  "session mode (bootloader)");

    // Seed 0x27/0x01, key 0x27/0x02 and jump 0x10/0x42, fatal.
    if (const Status unlocked = UnlockAndKernelJump(ExchangeContext(ctx), kRejectionPrefix, TcuCvtMitsuSeedKey);
        !unlocked.has_value())
    {
        return unlocked;
    }

    // Alive check. The SENT PDU is 0x34/0x04/0x33/0x00/0x00/0x00/0x08/0x00/0x00
    // -- the SAME literal trailing bytes as dump_flash_range's dump-setup
    // exchange and (by coincidence of this block's own halved data_len)
    // unlock_and_reflash_ block's setup exchange -- but the checked reply is
    // 0x71/0x02/0x02/0x03, the SID+0x40 form of a DIFFERENT service (0x31), not
    // 0x74 (0x34's own SID+0x40). Confirmed directly against the source,
    // re-read twice; the brief's Step 7 description of this step's sent bytes
    // as 0x31/0x02/0x02/0x01 (Task 3's own alive re-check PDU) does not match
    // the actual legacy source and is disclosed here as a found brief/source
    // discrepancy, not silently resolved either way. Because the reply's
    // service byte does not match the sent PDU's own SID, this cannot go
    // through FatalRequest()/ctx.uds (UdsClient:: request() would reject any
    // 0x71-led reply as "expected response to SID 0x34, got 0x71", making this
    // step always fail) -- routed through the channel directly instead.
    LogInfo(ctx, "Checking if jump successful and kernel alive...");
    if (const Status alive =
            RawExpect(ctx, bytes::Bytes{uds::kSidRequestDownload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
                      bytes::Bytes{0x71, uds::kRoutineControlStop, 0x02, 0x03}, "kernel alive check failed");
        !alive.has_value())
    {
        return alive;
    }

    LogInfo(ctx, "Kernel verified to be running");
    return {};
}

// "Sending stop command...": resent up to 6 times, content-blind (legacy checks
// only `received != ""`, never an SID or content match) -- and OUTCOME-blind
// too: legacy has no `if (try_count == 6) return ERROR` after this loop at all
// (unlike erase_mem's/unlock_and_reflash_block's own retry loops below, which
// do gate on the final try_count), so read_mem proceeds to decrypt and return
// success regardless of whether the TCU ever acknowledged stop. Routed through
// the channel directly, both because content is never checked and because a
// "failure" here must never fail the read.
Status SendStop(Ctx& ctx)
{
    LogInfo(ctx, "Sending stop command...");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (const Status sent = ctx.channel.Send(bytes::Bytes{uds::kSidRequestTransferExit}, ctx.cancellation);
            !sent.has_value())
        {
            return sent;
        }
        Result<std::optional<bytes::Bytes>> reply = ctx.channel.Receive(800ms, ctx.cancellation);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (reply->has_value())
        {
            break;
        }
    }
    return {};
}

Result<bytes::Bytes> DumpFlashRange(Ctx& ctx, PhaseReporter& progress)
{
    // "Settting dump start & length...". The sent PDU's SID is 0x35, but the
    // checked ack is 0x74 -- the same "set up transfer" ack byte
    // unlock_and_reflash_block's own (0x34-prefixed) setup exchange checks, not
    // the standard SID+0x40 response to 0x35 (which would be 0x75). The
    // trailing 6 bytes are a hardcoded literal, NOT derived from
    // kReadRegion.start/length (0x8000/0x78000) -- confirmed directly against
    // the source. Because the ack's service byte does not match the sent PDU's
    // own SID, this cannot go through FatalRequest()/ ctx.uds either -- routed
    // through the channel directly, mirroring the alive-check exchange's own
    // necessity in connect_bootloader.
    LogInfo(ctx, "Settting dump start & length...");
    return RawExpect(ctx, bytes::Bytes{uds::kSidRequestUpload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
                     bytes::Bytes{0x74, 0x20, 0x01, 0x04}, "dump start & length setup rejected")
        .and_then(
            [&]
            {
                return ReadEncryptedB7Pages(ExchangeContext(ctx), kRejectionPrefix, kReadRegion, TcuCvtMitsuDecryptPage,
                                            progress);
            })
        .and_then(
            [&](bytes::Bytes rom)
            {
                LogInfo(ctx, "ROM read complete");
                return SendStop(ctx).transform(
                    [&]
                    {
                        // Legacy pads the unread low 0x8000 region with 0xFF (the comment above
                        // the loop says "pad ... with 0x00" but the loop body writes 0xFF; the
                        // code, not the misleading comment, is preserved), unlike
                        // SubaruTcuCvtHitachiM32rCan, which pads with 0x00.
                        rom.insert(rom.begin(), kReadRegion.start, 0xFF);
                        return std::move(rom);
                    });
            });
}

// The literal legacy loop never resends (it writes the erase command once,
// above the loop, then only re-READS) and its mismatch branch's `return
// STATUS_ERROR` is commented out with no success branch anywhere -- so
// `try_count` always reaches 20 and the function ALWAYS returns STATUS_ERROR,
// regardless of what the TCU answers. That would make write_mem's very first
// step (`if (erase_mem()) { ...; return STATUS_ERROR; }`) fail unconditionally,
// so every write in this family has never worked in production. The brief's own
// description of this loop ("retried ... until <response> ... fatal after 20
// failed attempts") presupposes a working retry-until-match shape, matching the
// sibling close/checksum retry loops in this very file (both of which DO resend
// each attempt and DO break/return on an exact match). This targets that
// evident intent instead -- disclosed here as this family's one deliberate
// divergence from the literal legacy source, alongside Tasks 1 and 3's own
// disclosed findings.
Status EraseMemory(Ctx& ctx)
{
    LogInfo(ctx, "Erasing TCU ROM...");
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<bytes::Bytes> reply = ctx.uds.Request(
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff},
            kExchangePolicy, ctx.cancellation);
        if (reply.has_value())
        {
            if (const bytes::ByteView payload = uds::Payload(*reply);
                payload.size() >= 2 && payload[0] == uds::kRoutineControlStart && payload[1] == 0x02)
            {
                LogInfo(ctx, "Erased! Starting Writing! Do Not Power Off!");
                return {};
            }
            LogError(ctx, "Wrong response from TCU: unexpected erase acknowledgement");
        }
        else if (reply.error().kind == ErrorKind::kCancelled)
        {
            return std::unexpected(
                ReportExchangeFailure(ctx, reply.error(), "Wrong response from TCU: ", "the erase command"));
        }
        else
        {
            LogError(ctx, std::format("Wrong response from TCU: {}", reply.error().detail));
        }
    }
    LogError(ctx, "Flash area erase failed");
    return Fail(ErrorKind::kBadResponse, "flash area erase failed");
}

Status SendChunks(Ctx& ctx, bytes::ByteView encrypted, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;

    for (std::uint32_t offset = 0; offset < kWriteRegion.length; offset += kChunkSize)
    {
        // Legacy's stopRequested() check returns 0 -- success -- to the caller
        // on cancellation; the same bug Tasks 1 and 3 already disclosed and
        // diverged from. Not reproduced here either: a cancellation mid-write
        // is reported as Cancelled, never success.
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "write cancelled");
        }

        const std::uint32_t addr = kWriteRegion.start + offset;
        const bytes::ByteView chunk_data = encrypted.subspan(offset, kChunkSize);
        // Legacy reads but never inspects the per-chunk reply at all -- only a
        // transport-level failure from the read itself fails this exchange.
        // Routed through the channel directly, not FatalRequest()/ctx.uds,
        // which would enforce a SID+0x40 content match this family's legacy
        // code never checks here.
        if (const Status exchanged = ExchangeCvtB6Chunk(ctx.channel, ctx.cancellation, addr, chunk_data);
            !exchanged.has_value())
        {
            return exchanged;
        }
        progress.Update(static_cast<int>(offset + kChunkSize));
    }
    return {};
}

// "Verifying checksum...": retried up to 20 times, fatal after 20 -- the same
// shape as erase_mem's own (fixed) retry loop above.
Status VerifyChecksum(Ctx& ctx)
{
    LogInfo(ctx, "Verifying checksum...");
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<bytes::Bytes> checksum =
            ctx.uds.Request(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x02, 0x01},
                            kExchangePolicy, ctx.cancellation);
        if (checksum.has_value())
        {
            if (const bytes::ByteView payload = uds::Payload(*checksum);
                payload.size() >= 2 && payload[0] == uds::kRoutineControlStart && payload[1] == 0x02)
            {
                LogInfo(ctx, "Checksum verified...");
                return {};
            }
            LogError(ctx, "Wrong response from TCU: ROM checksum error");
        }
        else
        {
            LogError(ctx, std::format("Wrong response from TCU: {}", checksum.error().detail));
        }
    }
    return Fail(ErrorKind::kBadResponse, "ROM checksum verify failed");
}

// Called once (this family flashes exactly one block, kWriteRegion /
// kFlashBlocksMH8111[3]).
Status UnlockAndReflashBlock(Ctx& ctx, bytes::ByteView image, PhaseReporter& progress)
{
    const bytes::ByteView block_plain = image.subspan(kWriteRegion.start, kWriteRegion.length);
    constexpr std::uint32_t kChunkSize = 256;

    // maxblocks/end_addr/data_len: maxblocks = pl_len/256 (the real chunk count
    // the loop below uses -- 4096 for this block's 0x100000 bytes), but
    // end_addr is computed as start + maxblocks*128 (HALF a chunk's width per
    // block, not *256), so data_len == pl_len/2 == 0x80000 for this block. This
    // is a genuine legacy arithmetic bug in the value DECLARED in the setup
    // packet below -- distinct from erase_mem's control-flow bug above, this
    // one does not prevent the write from working (the chunk loop below still
    // sends the full 0x100000 bytes; only the setup packet's own length field
    // is wrong) -- so it is preserved exactly rather than corrected, per this
    // package's established precedent for found-but-harmless byte-value quirks
    // (Task 3's block-size finding).
    constexpr std::uint32_t kMaxBlocks = kWriteRegion.length / kChunkSize; // 4096
    constexpr std::uint32_t kSetupDataLen = kMaxBlocks * 128;              // 0x80000

    // "Settting flash start & length...": retried up to 6 times, fatal after 6.
    // Legacy DOES check content here (`received.at(4) == 0x74`, confirmed
    // directly against the source -- the brief's Step 7 description of this
    // step as content-blind does not match), but that check is exactly the
    // standard SID+0x40 match ctx.uds.request() already enforces (0x34 ->
    // 0x74), so no extra check is added here beyond the retry-count wrapper.
    LogInfo(ctx, "Settting flash start & length...");
    if (!RequestWithRetries(ctx, ComposeBe(uds::kSidRequestDownload, 0x04_b, 0x33_b, U24{0}, U24{kSetupDataLen}), 6))
    {
        return Fail(ErrorKind::kBadResponse, "flash start & length setup rejected");
    }
    if (const Status sent = SendChunks(ctx, TcuCvtMitsuEncryptRom(block_plain), progress); !sent.has_value())
    {
        return sent;
    }
    // Closing: retried up to 20 times, fatal after 20 -- unlike Task 1's ECU
    // family (6-attempt, tolerant-of-anything-including- failure close) and
    // Task 3's own close (single fatal attempt, no retry). Content beyond the
    // standard SID+0x40 match (0x37 -> 0x77) is never checked, matching
    // ctx.uds.request()'s own enforcement exactly.
    LogInfo(ctx, "Closing out Flashing of this block...");
    if (!RequestWithRetries(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, 20))
    {
        return Fail(ErrorKind::kBadResponse, "block close failed");
    }
    LogInfo(ctx, "Flashing of block closed");
    return VerifyChecksum(ctx);
}

} // namespace

Result<Iso15765Config> SubaruTcuCvtMitsuMh8111CanExecutor::Setup(const SubaruTcuCvtMitsuMh8111CanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruTcuCvtMitsuMh8111CanExecutor::Run(const SubaruTcuCvtMitsuMh8111CanPlan& family,
                                                                     const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};
    return RunBootloaderOperation(plan, "Subaru TCU CVT Mitsu MH8111", ctx, ConnectBootloader, DumpFlashRange,
                                  EraseMemory, UnlockAndReflashBlock);
}

} // namespace fastecu::flash
