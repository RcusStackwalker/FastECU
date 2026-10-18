#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_executor.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include <chrono>
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh72543_can_diesel_operation.cpp
// it was transcribed from, as of 20892df -- this branch's base, and the last
// commit before the file was deleted here. Master later reflowed one line in
// three of the four (S1117, #240), so resolve citations against 20892df.
namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

// Legacy's read timeouts, kept apart rather than flattened into one policy:
// they are this family's own wire timing (header lines 46-52). Where its
// 1N83M 1.5M and SH72531 siblings read with the 200 ms
// serial_read_short_timeout, this family mostly reads with the 2000 ms
// serial_read_timeout -- see the per-exchange citations below. The third,
// receive_timeout (500), is now read only by the shared read stop in
// denso_iso15765_can_common.
constexpr std::chrono::milliseconds kShortTimeout{200}; // serial_read_short_timeout
constexpr std::chrono::milliseconds kLongTimeout{2000}; // serial_read_timeout
constexpr uds::ExchangePolicy kShortPolicy{.read_timeout = kShortTimeout};
constexpr uds::ExchangePolicy kLongPolicy{.read_timeout = kLongTimeout};
// The timeout the tolerant_probe exchanges in connect_bootloader read with:
// serial_read_timeout -- THIS FAMILY DIFFERS from its three siblings, which probe with serial_read_short_timeout.
// Named rather than repeated at each call so the one line a reader has to
// compare across the four families is this one.
constexpr std::chrono::milliseconds kProbeTimeout = kLongTimeout;
// Checksum verify reads twice and this family uses serial_read_timeout for
// both: the first read at line 1328 and, after the ECU's 7F 31 78 pending
// answer, the re-read at line 1348. UdsClient substitutes pending_timeout
// for that second read, whose 3000 ms default is not this family's number, so
// the policy pins it to 2000 as well.
constexpr uds::ExchangePolicy kChecksumPolicy{.read_timeout = kLongTimeout, .pending_timeout = kLongTimeout};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band;
// legacy uses its own values here rather than the standard subfunctions.
constexpr bytes::Byte kSessionBench = 0x43;     // bench programming session
constexpr bytes::Byte kSessionBenchJump = 0x42; // bench jump to on-board kernel
// Positive-response service ids for the SIDs this file inspects without going
// through UdsClient (which does the SID + 0x40 arithmetic itself).
constexpr bytes::Byte kSessionControlReply = uds::kSidDiagnosticSessionControl + 0x40; // 0x50

// ISO 14229-1 dataIdentifier 0xF182 (systemSupplierECUSoftwareNumber band).
// Unlike its three siblings, which query the ECU id with the vendor SID 0xAA,
// this family reads it with a standard ReadDataByIdentifier (lines 131-140)
// and checks the whole echoed `62 F1 82` header (line 146).
constexpr bytes::Byte kDidEcuIdHigh = 0xF1;
constexpr bytes::Byte kDidEcuIdLow = 0x82;

// Byte 0 of the plan image is this flash address. Legacy write_memory hands
// reflash_block &data_array[fdt->fblocks->start] (line 1129), i.e. the
// encrypted whole-ROM buffer offset by fblocks[0].start = 0x8000, and
// reflash_block then indexes newdata[i + blockctr * blocksize] (line 1217) --
// which together read data_array[blockaddr + i]: the absolute address,
// matching read_memory's own image base and the three sibling families. The
// original port design's "write-base divergence" section reads line 1217
// alone and concludes legacy wrote a full ROM 0x8000 low; with the caller's
// pre-offset it does not. Do not "restore" a 0x8000 shift on its strength.
constexpr std::uint32_t kImageStart = 0x00000000;

// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

// The ECU ID query needs a two-byte reply check (`F1 82`, line 146) where the
// shared non_fatal_query checks one subfunction byte, so this family carries
// its own non-fatal variant. Same shape otherwise: log on a match, log and
// continue on anything else.
void NonFatalDidQuery(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_did, std::string_view label)
{
    Result<bytes::Bytes> reply = ctx.uds.Request(pdu, kLongPolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, reply.error().detail));
        return;
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.size() < expected_did.size() || !std::equal(expected_did.begin(), expected_did.end(), payload.begin()))
    {
        LogError(ctx, std::format("{}unexpected {} response", kDensoIso15765RejectionPrefix, label));
        return;
    }
    // Legacy trims the raw frame with remove(0, 7) -- the 4-byte envelope plus
    // `62 F1 82` (line 149) -- and although its remove(5, len-5) truncation is
    // commented out at line 150, the loop that builds the logged string runs
    // `for (int i = 0; i < 5; i++)` (lines 153-156), so exactly five bytes are
    // logged. Take the same five. Fewer are taken only if the reply is short,
    // where legacy would index past the end instead.
    constexpr std::size_t kEcuIdBytes = 5;
    const bytes::ByteView rest = payload.subspan(expected_did.size());
    LogInfo(ctx, std::format("{}: {}", label, bytes::ToHex(rest.subspan(0, std::min(kEcuIdBytes, rest.size())))));
}

// Bench programming arm, legacy lines 649-791.
Status ConnectBench(Ctx& ctx)
{
    LogInfo(ctx, "Bench programming: accessing, please wait...");
    // Line 653. Its in-car counterpart waits 500 ms (line 345); this arm waits
    // 50, and both precede a write, so neither is subsumed by a read timeout.
    if (const Status slept = ctx.clock.Sleep(50ms, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 655-681. This is the family's one commented-out
    // `return STATUS_ERROR` (line 672): a wrong `50 43` answer is logged and
    // stepped over, and only an absent or too-short reply aborts. Its three
    // sibling families abort here, so this tolerance is transcribed
    // deliberately rather than normalized away.
    if (Result<bytes::Bytes> session =
            TolerantProbe(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBench}, kSessionControlReply,
                          kSessionBench, kProbeTimeout, kDensoIso15765RejectionPrefix, "bench diagnostic session");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 682-753.
    if (const Status unlocked = DensoSecurityAccess(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    return DensoIso15765JumpToKernel(ctx, kSessionBenchJump, 50, kLongTimeout, kShortTimeout,
                                     /*discard_first_reply=*/false);
}

// Legacy connect_bootloader, lines 92-797. The legacy `is_serial_port_open()`
// guard (lines 98-104) has no equivalent: execute() below opens the transport
// through the port and propagates that Status, so an unopened bus fails
// earlier and with a typed error.
Status ConnectBootloader(Ctx& ctx, ICanFlashTransport& can)
{
    // OBK-already-active probe (lines 108-128). A match short-circuits the
    // whole sequence; a mismatch or an empty read falls through.
    // The only exchange in the connect sequence that reads with
    // serial_read_short_timeout (line 118).
    Result<bool> obk_active = DensoObkIsActive(ctx, "OBK is active!");
    if (!obk_active.has_value())
    {
        return std::unexpected(obk_active.error());
    }
    if (*obk_active)
    {
        return {};
    }

    // Lines 131-282: four non-fatal identity queries.
    LogInfo(ctx, "Requesting ECU ID");
    NonFatalDidQuery(ctx, bytes::Bytes{uds::kSidReadDataByIdentifier, kDidEcuIdHigh, kDidEcuIdLow},
                     bytes::Bytes{kDidEcuIdHigh, kDidEcuIdLow}, "ECU ID");
    DensoVehicleInfoQueries(ctx);

    // Lines 284-342: the tolerant access-method probe and branch selector.
    Result<bool> in_car = DensoProgrammingBranchIsInCar(ctx, kProbeTimeout);
    if (!in_car.has_value())
    {
        return std::unexpected(in_car.error());
    }
    return *in_car ? DensoIso15765ConnectInCar(ctx, can, DensoInCarRun::kSh, kLongTimeout) : ConnectBench(ctx);
}

// Legacy's own log line for all three "start & length" announcements
// (lines 822-824, 1191-1193, 1396-1398). QString::number(x, 16) is lowercase
// and unpadded, so this reproduces it character for character.
std::string StartAndLengthLine(const MemoryRegion& region)
{
    return std::format("Setting flash start: 0x{:x} and length: 0x{:x}", region.start, region.length);
}

// Legacy read_memory, lines 800-1062. Its `start_addr = 0x8000` /
// `length = 0x001F7F00` overwrite is commented out (lines 813-814), so unlike
// its three siblings this family's execute() arguments -- fblocks[0].start and
// fblocks[0].len -- actually reach it. The plan carries exactly that region.
Result<bytes::Bytes> ReadMemory(Ctx& ctx, const SubaruDensoSh72543CanDieselPlan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    // Lines 822-901: both setup exchanges are fatal on a mismatch and on an
    // absent reply, and both read with serial_read_short_timeout (lines 845,
    // 884) -- the two reads in this family that do not use the long timeout.
    LogInfo(ctx, StartAndLengthLine(region));
    if (Result<bytes::Bytes> download =
            FatalQuery(ctx, DensoIso15765DownloadSetupPdu(region), bytes::Bytes{0x20, 0x01, 0x05}, kShortPolicy,
                       "dump start & length setup");
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (Result<bytes::Bytes> upload = FatalQuery(ctx, DensoIso15765UploadSetupPdu(region),
                                                 bytes::Bytes{0x20, 0x01, 0x01}, kShortPolicy, "dump upload setup");
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    return DensoIso15765ReadDump(ctx, region, family.lead_pad_len, family.tail_pad_len, progress,
                                 /*pace_pages=*/true);
}

// Legacy reflash_block, lines 1156-1368, called for block 0 only.
Status ReflashBlock(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    LogInfo(ctx, StartAndLengthLine(block));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, kImageStart, block, progress, kChecksumPolicy);
}

// Legacy write_memory, lines 1070-1149. block_modified is {1} over
// numblocks == 1, so block 0 -- kFlashBlocksSH72543d[0], the plan's transfer
// region -- is the one block erased and reflashed. Its three siblings all
// flash block 1 of a three-block table; this family's table has a single
// block because its 0x0-0x8000 entry is commented out.
Status WriteMemory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    return DensoIso15765WriteBlock(ctx, block, 0, phases,
                                   [&](PhaseReporter& write) { return ReflashBlock(ctx, image, block, write); });
}

} // namespace

Result<Iso15765Config> SubaruDensoSh72543CanDieselExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruDensoSh72543CanDiesel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruDensoSh72543CanDieselPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruDensoSh72543CanDieselPlan>(plan.FamilyPlan());
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruDensoSh72543CanDieselExecutor::Execute(const FlashPlan& plan,
                                                                          ICanFlashTransport& transport, IClock& clock,
                                                                          const ICancellationToken& cancellation,
                                                                          IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh72543CanDiesel); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh72543CanDieselPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDensoSh72543CanDieselPlan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start("Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    LogInfo(ctx, "Connecting to ECU Denso SH72543 Diesel CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(ctx, transport); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (read)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, "Reading ROM from ECU, Denso SH72543 Diesel using CAN");

        PhaseReporter read_phase = phases.Start("Read ROM", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = ReadMemory(ctx, family, plan.TransferRegion(), read_phase);
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

    // build_subaru_denso_sh72543_can_diesel_plan refuses TestWrite; the guard
    // is repeated here so a plan built another way cannot turn a dry run into
    // a real erase and write. Legacy threaded test_write all the way down to
    // reflash_block and then never consulted it.
    //
    // It cannot fire as the code stands: the plan validation at the top of
    // execute() rejects TestWrite before any I/O, and the Read branch has
    // already returned, so FlashOperation has no third value left to reach
    // here. The bootloader-dialect factoring pass reviewed it across all four
    // families and kept it: it costs nothing at runtime and is the last
    // thing between a non-Write operation and a real erase-and-write of an
    // ECU should that entry validation ever be relaxed or the enum gain a
    // value. Do not delete it as "dead code".
    if (plan.Operation() != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported,
                    "test_write is not supported by the Subaru Denso SH72543 Diesel CAN family");
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, "Writing ROM to ECU, Denso SH72543 Diesel using CAN");
    if (const Status written = WriteMemory(ctx, plan.ImageOrEmpty(), plan.TransferRegion(), phases);
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.Operation(),
        .read_bytes = std::nullopt,
    };
}

} // namespace fastecu::flash
