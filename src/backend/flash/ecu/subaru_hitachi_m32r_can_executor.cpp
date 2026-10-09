#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_executor.h"

#include <array>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_hitachi_m32r_can_operation.cpp
// it was transcribed from.
namespace fastecu::flash
{
namespace
{
using namespace bytes::literals;
using namespace std::chrono_literals;
using bytes::ComposeBe;
using bytes::U24;

constexpr uds::ExchangePolicy kExchangePolicy{.read_timeout = 500ms};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific
// band -- unlike uds::kSessionProgramming/kSessionExtendedDiagnostic
// (subaru_tcu_cvt_hitachi_m32r_can_executor.cpp uses those standard
// values), this family's legacy source uses its own bench-only ids here.
constexpr bytes::Byte kSessionBench = 0x43;
constexpr bytes::Byte kSessionKernelJump = 0x42;

constexpr auto kSeedKeyTable =
    std::to_array<std::uint16_t>({0x90A1, 0x2F92, 0xDE3C, 0xCDC0, 0x1A99, 0x437C, 0xF91B, 0xDB57, 0x96BA, 0xDE10,
                                  0xFCAF, 0x3F31, 0xF47F, 0x0BB6, 0x16E9, 0x4645});
constexpr auto kEncryptTable = std::to_array<std::uint16_t>({0x14CA, 0x77F4, 0x973C, 0xF50E});
constexpr auto kDecryptTable = std::to_array<std::uint16_t>({0xF50E, 0x973C, 0x77F4, 0x14CA});

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kSeedKeyTable, ssm_protocol::kIndexTransformationStock);
}

bytes::Bytes EncryptRom(bytes::ByteView image)
{
    return ssm_protocol::CalculatePayload(image, static_cast<std::uint32_t>(image.size()), kEncryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

bytes::Bytes DecryptPage(bytes::ByteView page)
{
    return ssm_protocol::CalculatePayload(page, static_cast<std::uint32_t>(page.size()), kDecryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

// Every exchange goes through UdsClient over CanFlashUdsChannel except two
// (the OBK-already-running probe and the session-scope probe) whose response
// format the branch decisions below depend on does not follow the
// SID+0x40/NRC-decoding convention UdsClient enforces, so those two go
// through the channel directly. `channel` is the same CanFlashUdsChannel
// instance `uds` wraps.
// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

constexpr std::string_view kRejectionPrefix = "Wrong response from ECU: ";

UdsExchangeContext ExchangeContext(Ctx& ctx)
{
    return UdsExchangeContext{ctx.uds, kExchangePolicy, ctx.cancellation, ctx.events};
}

// The "fatal" shape every exchange below uses except the four identity
// queries (non_fatal_query) and the close-block retry loop (each
// intentionally tolerant of failure, see below). See
// uds_client_exchange_common.h for the shared rejection/cancellation
// logging this delegates to.
Result<bytes::Bytes> FatalRequest(Ctx& ctx, bytes::ByteView pdu, std::string_view operation)
{
    return ::fastecu::flash::FatalRequest(ExchangeContext(ctx), pdu, kRejectionPrefix, operation);
}

// The fatal_request + expected-response-prefix check every exchange below
// repeats except the four identity queries and the close-block retry loop --
// see uds_client_exchange_common.h's fatal_query for what expected_prefix,
// subject, and min_payload_size mean.
Result<bytes::Bytes> FatalQuery(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                                std::string_view subject, std::optional<std::size_t> min_payload_size = std::nullopt)
{
    return ::fastecu::flash::FatalQuery(ExchangeContext(ctx), pdu, expected_prefix, kRejectionPrefix, subject,
                                        min_payload_size);
}

// Legacy's four non-fatal identity queries (ECU ID/VIN/CAL ID/CVN, lines
// 115-257): each is info-logged on a matching reply and error-logged
// otherwise, but the connect sequence never halts here -- even a genuine
// exchange failure (timeout, disconnect) is logged and swallowed, exactly
// mirroring legacy's total absence of early returns in this block.
void NonFatalQuery(Ctx& ctx, bytes::ByteView request_pdu, std::optional<bytes::Byte> expected_subfunction,
                   std::string_view label)
{
    ::fastecu::flash::NonFatalQuery(ExchangeContext(ctx), request_pdu, expected_subfunction, kRejectionPrefix, label);
}

// Legacy connect_bootloader, flash_ecu_subaru_hitachi_m32r_can_operation.cpp:76-760.
// The legacy `is_serial_port_open()` guard (lines 85-89) has no equivalent:
// execute() below opens the transport through the port and propagates that
// Status, so an unopened bus fails earlier and with a typed error.
Status ConnectBootloader(Ctx& ctx)
{
    // "Checking if OBK is already running..." (lines 91-113). The
    // already-running signal is packaged as a UDS negative response (0x7F
    // 0xB7 0x13) that UdsClient::request() would otherwise treat as an
    // ordinary exchange failure, so this one goes through the channel
    // directly rather than through ctx.uds -- and, matching legacy exactly,
    // a mismatch OR an empty/failed read is a non-fatal "miss" that falls
    // through to initializing the ECU rather than propagating.
    LogInfo(ctx, "Checking if OBK is already running...");
    if (const Status sent = ctx.channel.Send(bytes::Bytes{0xB7}, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> obk = ctx.channel.Receive(200ms, ctx.cancellation);
    if (!obk.has_value())
    {
        return std::unexpected(obk.error());
    }
    if (obk->has_value() && obk->value().size() > 2 && (**obk)[0] == 0x7F && (**obk)[1] == 0xB7 && (**obk)[2] == 0x13)
    {
        LogInfo(ctx, "OBK is active!");
        return {};
    }
    LogInfo(ctx, "OBK not active, initializing ECU...");

    // Lines 115-257: four non-fatal identity queries.
    LogInfo(ctx, "Requesting ECU ID");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidEcuIdQuery}, std::nullopt, "ECU ID");
    LogInfo(ctx, "Requesting VIN");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidVin}, uds::kVehicleInfoPidVin,
                  "VIN");
    LogInfo(ctx, "Requesting CAL ID...");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, uds::kVehicleInfoPidCalId,
                  "CAL ID");
    LogInfo(ctx, "Requesting CVN");
    NonFatalQuery(ctx, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCvn}, uds::kVehicleInfoPidCvn,
                  "CVN");

    // Session-scope probe (lines 259-279): also a raw channel exchange,
    // because the branch decision below inspects payload[1]/payload[2]
    // without the response ever carrying a SID+0x40 echo of the request.
    LogInfo(ctx, "Initializing bootloader...");
    if (const Status sent = ctx.channel.Send(bytes::Bytes{0xA8, 0x00, 0x00, 0x00, 0xD7}, ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> probe = ctx.channel.Receive(200ms, ctx.cancellation);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (!probe->has_value() || probe->value().size() < 3)
    {
        // Legacy's outer `if (received.length() > 5) {...} return
        // STATUS_ERROR;` (line 759): this is the only failure point in
        // connect_bootloader with no dedicated legacy log line, and the
        // first point that DOES propagate rather than falling through.
        return Fail(ErrorKind::kTimeout, "no response from ECU during the session-scope probe");
    }
    const bytes::Bytes& session_probe = **probe;
    // `at(5) != 0xA0 && at(6) != 0x20` selects on-car; De Morgan's gives
    // bench whenever either byte matches.
    if (const bool bench = session_probe[1] == 0xA0 || session_probe[2] == 0x20; !bench)
    {
        // On-car programming branch (lines 281-579): out of scope per the
        // design's on-car scope decision (only the bench path is ported).
        return Fail(ErrorKind::kUnsupported, "on-car programming is not supported by this port");
    }
    LogInfo(ctx, "Bench Programming, Accessing...");

    // Bench branch (lines 581-753): every exchange from here on is fatal on
    // mismatch or empty reply, unlike the identity queries above.
    if (Result<bytes::Bytes> session = FatalQuery(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBench},
                                                  bytes::Bytes{kSessionBench}, "bench diagnostic session");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    LogInfo(ctx, "Starting seed request...");
    Result<bytes::Bytes> seed_reply =
        FatalQuery(ctx, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
                   bytes::Bytes{uds::kSecurityAccessRequestSeed}, "seed request", 5);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    LogInfo(ctx, "Seed request ok");
    const bytes::ByteView seed_payload = uds::Payload(*seed_reply);
    const bytes::ByteView seed = seed_payload.subspan(1, 4);
    LogInfo(ctx, std::format("Received seed: {}", bytes::ToHex(seed)));
    const bytes::Bytes key = SeedKey(seed);
    LogInfo(ctx, std::format("Calculated seed key: {}", bytes::ToHex(key)));

    LogInfo(ctx, "Sending seed key to ECU...");
    if (Result<bytes::Bytes> key_reply =
            FatalQuery(ctx, ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, key),
                       bytes::Bytes{uds::kSecurityAccessSendKey}, "seed key");
        !key_reply.has_value())
    {
        return std::unexpected(key_reply.error());
    }
    LogInfo(ctx, "Seed key ok");

    LogInfo(ctx, "Jumping to onboard kernel...");
    if (Result<bytes::Bytes> jump_reply =
            FatalQuery(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionKernelJump},
                       bytes::Bytes{kSessionKernelJump}, "kernel jump");
        !jump_reply.has_value())
    {
        return std::unexpected(jump_reply.error());
    }
    LogInfo(ctx, "Jump to kernel ok");

    LogInfo(ctx, "Checking if jump successful and kernel alive...");
    if (Result<bytes::Bytes> alive_reply =
            FatalQuery(ctx, bytes::Bytes{uds::kSidRequestDownload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
                       bytes::Bytes{0x20, 0x01, 0x04}, "kernel alive check");
        !alive_reply.has_value())
    {
        return std::unexpected(alive_reply.error());
    }

    LogInfo(ctx, "ECU initialised, continue...");
    return {};
}

// Legacy read_mem, lines 767-963. start_addr/length are hardcoded to
// 0/0x80000 in legacy (lines 780-781) regardless of caller arguments; this
// family's plan always carries that exact transfer_region, so the sweep
// below is written as the fixed full-ROM sweep legacy actually performs.
Result<bytes::Bytes> DumpFlashRange(Ctx& ctx, PhaseReporter& progress)
{
    constexpr std::uint32_t kPageSize = 0x100;
    constexpr std::uint32_t kLength = 0x80000;

    // "Settting dump start & length..." (lines 788-819): non-fatal on
    // mismatch or empty reply -- legacy only logs here, never returns early.
    LogInfo(ctx, "Settting dump start & length...");
    if (Result<bytes::Bytes> setup =
            ctx.uds.Request(bytes::Bytes{uds::kSidRequestUpload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
                            kExchangePolicy, ctx.cancellation);
        !setup.has_value())
    {
        LogError(ctx, std::format("Wrong response from ECU: {}", setup.error().detail));
    }
    else if (const bytes::ByteView p = uds::Payload(*setup);
             p.size() < 3 || p[0] != 0x20 || p[1] != 0x01 || p[2] != 0x01)
    {
        LogError(ctx, "Wrong response from ECU: unexpected dump setup response");
    }

    LogInfo(ctx, "Start reading ROM, please wait...");
    bytes::Bytes rom;
    rom.reserve(kLength);
    for (std::uint32_t addr = 0; addr < kLength; addr += kPageSize)
    {
        // Legacy stopRequested() at line 840, top of loop.
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "read cancelled");
        }

        // Lines 823-832/855-858: SID 0xB7 + 3-byte big-endian address.
        Result<bytes::Bytes> chunk = FatalRequest(ctx, ComposeBe(uds::kSidReadMemoryChunk, U24(addr)),
                                                  std::format("the flash read at 0x{:x}", addr));
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        // Lines 877-881: the 256-byte encrypted page is everything after the
        // SID, decrypted in place.
        const bytes::Bytes decrypted = DecryptPage(uds::Payload(*chunk));
        rom.insert(rom.end(), decrypted.begin(), decrypted.end());
        progress.Update(static_cast<int>(addr + kPageSize));
    }

    LogInfo(ctx, "ROM read complete");

    // "Sending stop command..." (lines 928-955): fatal on mismatch or empty
    // reply, unlike the dump-setup exchange above.
    LogInfo(ctx, "Sending stop command...");
    if (Result<bytes::Bytes> stop = FatalRequest(ctx, bytes::Bytes{uds::kSidRequestTransferExit}, "the stop command");
        !stop.has_value())
    {
        return std::unexpected(stop.error());
    }

    return rom;
}

// Legacy erase_memory, lines 1279-1345. One write, then re-reads (never
// re-sends) up to 21 times total until the ECU reports completion --
// structurally the CAN counterpart of subaru_hitachi_m32r_kline_executor.cpp's
// erase_rom retry loop.
Status EraseMemory(Ctx& ctx)
{
    LogInfo(ctx, "Erasing ECU ROM...");
    if (const Status sent = ctx.channel.Send(
            bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x01, 0x0f, 0xff, 0xff, 0xff},
            ctx.cancellation);
        !sent.has_value())
    {
        return sent;
    }
    // Legacy's initial post-write read (lines 1310-1311) is discarded --
    // unconditionally overwritten by the loop's first iteration -- so it is
    // not reproduced as a separate step here.
    bool connected = false;
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(500ms, ctx.cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        // 0x71 is kSidRoutineControl's own positive-response echo
        // (SID + 0x40); byte[1] echoes the sent kRoutineControlStart verb.
        if (received->has_value() && received->value().size() > 2 && (**received)[0] == 0x71 &&
            (**received)[1] == uds::kRoutineControlStart && (**received)[2] == 0x02)
        {
            connected = true;
            break;
        }
        if (const Status slept = ctx.clock.Sleep(500ms, ctx.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    if (!connected)
    {
        LogError(ctx, "Flash area erase failed");
        return Fail(ErrorKind::kBadResponse, "flash area erase failed");
    }
    LogInfo(ctx, "Flash erased! Starting flash write, do not power off!");
    return {};
}

// Legacy reflash_block, lines 1056-1272, called once for the whole ROM
// (numblocks == 1 for this family's FlashDevice).
Status UnlockAndReflashBlock(Ctx& ctx, bytes::ByteView image, PhaseReporter& progress)
{
    constexpr std::uint32_t kChunkSize = 256;
    constexpr std::uint32_t kLength = 0x80000;

    // "Setting flash start & length..." (lines 1092-1127): fatal on mismatch
    // or empty reply.
    LogInfo(ctx, "Setting flash start & length...");
    if (Result<bytes::Bytes> setup =
            FatalRequest(ctx, bytes::Bytes{uds::kSidRequestDownload, 0x04, 0x33, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00},
                         "the flash start & length setup");
        !setup.has_value())
    {
        return std::unexpected(setup.error());
    }

    // The whole ROM is encrypted once up front (legacy write_mem line 986),
    // not per chunk.
    const bytes::Bytes encrypted = EncryptRom(image);

    for (std::uint32_t addr = 0; addr < kLength; addr += kChunkSize)
    {
        // Legacy's stopRequested() check (line 1132) returns 0 -- success --
        // to the caller on cancellation, which write_mem then reports as a
        // completed block. That is not reproduced here: a cancellation
        // mid-write must never be reported as a successful ROM write, so
        // this deliberately diverges from the literal legacy return value
        // and reports Cancelled like every other family's write loop.
        if (ctx.cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "write cancelled");
        }

        const bytes::ByteView chunk_data = bytes::ByteView(encrypted).subspan(addr, kChunkSize);
        if (Result<bytes::Bytes> chunk = FatalRequest(ctx, ComposeBe(uds::kSidWriteMemoryChunk, U24(addr), chunk_data),
                                                      std::format("the flash write at 0x{:x}", addr));
            !chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        progress.Update(static_cast<int>(addr + kChunkSize));
    }

    // Close-block retry loop (legacy lines 1180-1210): up to 6 attempts,
    // tolerant of any non-0x77 response -- including a genuine exchange
    // failure -- and the loop's own success flag is never checked
    // afterward. Transcribed exactly: no check is added here that legacy
    // does not have.
    LogInfo(ctx, "Closing out Flashing of this block...");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        if (Result<bytes::Bytes> closed =
                ctx.uds.Request(bytes::Bytes{uds::kSidRequestTransferExit}, kExchangePolicy, ctx.cancellation);
            closed.has_value())
        {
            LogInfo(ctx, "Closed succesfully");
            break;
        }
    }

    // "Verifying checksum..." (legacy lines 1214-1269). The ECU answers
    // pending (0x7F 0x31 0x78) before the real result; UdsClient absorbs
    // that NRC by re-reading internally, so this is a single request() call.
    LogInfo(ctx, "Verifying checksum...");
    if (Result<bytes::Bytes> checksum =
            FatalQuery(ctx, bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02, 0x02, 0x01},
                       bytes::Bytes{0x01, 0x02}, "checksum verify");
        !checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    LogInfo(ctx, "Checksum verified");
    return {};
}

// Legacy write_mem, lines 971-1049. numblocks == 1 for this family, so this
// is one erase_memory() call followed by exactly one unlock_and_reflash_block
// call over the full ROM -- no per-block loop.
Status WriteMem(Ctx& ctx, bytes::ByteView image, PhaseSequence& phases)
{
    PhaseReporter erase = phases.Start("Erase", 1);
    if (const Status erased = EraseMemory(ctx); !erased.has_value())
    {
        return erased;
    }
    erase.Complete();

    PhaseReporter write = phases.Start("Write ROM", static_cast<int>(image.size()));
    if (const Status written = UnlockAndReflashBlock(ctx, image, write); !written.has_value())
    {
        return written;
    }
    write.Complete();
    return {};
}

} // namespace

Result<Iso15765Config> SubaruHitachiM32rCanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruHitachiM32rCan); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruHitachiM32rCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruHitachiM32rCanPlan>(plan.FamilyPlan());
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruHitachiM32rCanExecutor::Execute(const FlashPlan& plan, ICanFlashTransport& transport,
                                                                   IClock& clock,
                                                                   const ICancellationToken& cancellation,
                                                                   IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruHitachiM32rCan); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruHitachiM32rCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruHitachiM32rCanPlan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start(read ? "Connect to ECU" : "Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    LogInfo(ctx, "Connecting to Subaru ECU 512Kb Hitachi CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(ctx); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (plan.Operation() == FlashOperation::kRead)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, "Reading ROM from ECU Subaru using CAN");

        PhaseReporter read_phase = phases.Start("Read ROM", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = DumpFlashRange(ctx, read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead,
            .read_bytes = std::move(*rom),
        };
    }

    // build_subaru_hitachi_m32r_can_plan refuses TestWrite; the guard is
    // repeated here so a plan built another way cannot turn a dry run into a
    // real erase and write.
    if (plan.Operation() != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported, "test_write is not supported by the Subaru Hitachi M32R CAN family");
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, "Writing ROM to Subaru Hitachi WA12212970WWW using CAN");
    if (const Status written = WriteMem(ctx, plan.ImageOrEmpty(), phases); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.Operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
