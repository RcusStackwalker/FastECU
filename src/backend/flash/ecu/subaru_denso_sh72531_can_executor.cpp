#include "src/backend/flash/ecu/subaru_denso_sh72531_can_executor.h"

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
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_sh72531_can_operation.cpp
// it was transcribed from, as of 20892df -- this branch's base, and the last
// commit before the file was deleted here. Master later reflowed one line in
// three of the four (S1117, #240), so resolve citations against 20892df.
namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using namespace std::chrono_literals;

// Legacy's read timeouts, kept apart rather than flattened into one policy:
// they are this family's own wire timing (header lines 45-48). Legacy
// reaches most of them through bare literals rather than the named members
// -- 200 at lines 671/703/748/782/785/1278 -- which is why the values, not
// the names, are authoritative here. The third, receive_timeout (500), is now
// read only by the shared read stop in denso_iso15765_can_common.
constexpr std::chrono::milliseconds kShortTimeout{200}; // serial_read_short_timeout
constexpr std::chrono::milliseconds kLongTimeout{2000}; // serial_read_timeout
constexpr uds::ExchangePolicy kShortPolicy{.read_timeout = kShortTimeout};
constexpr uds::ExchangePolicy kLongPolicy{.read_timeout = kLongTimeout};
// The timeout the tolerant_probe exchanges in connect_bootloader read with:
// serial_read_short_timeout, as in two of its three siblings.
// Named rather than repeated at each call so the one line a reader has to
// compare across the four families is this one.
constexpr std::chrono::milliseconds kProbeTimeout = kShortTimeout;
// Checksum verify reads twice and this family uses serial_read_timeout for
// both: the first read at line 1310 and, after the ECU's 7F 31 78 pending
// answer, the re-read at line 1330. UdsClient substitutes pending_timeout
// for that second read, whose 3000 ms default is not this family's number, so
// the policy pins it to 2000 as well.
constexpr uds::ExchangePolicy kChecksumPolicy{.read_timeout = kLongTimeout, .pending_timeout = kLongTimeout};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band;
// legacy uses its own values here rather than the standard subfunctions.
constexpr bytes::Byte kSessionBench = 0x43;     // bench programming session
constexpr bytes::Byte kSessionBenchJump = 0x42; // bench jump to on-board kernel
// kFlashBlocksSH72531[0].start. write_memory hands reflash_block
// &data_array[fdt->fblocks->start] (line 1140), i.e. fblocks[0].start, and
// reflash_block then indexes newdata[i + blockaddr] (line 1225) -- so byte 0
// of the plan image is this address, which for SH72531 is 0.
constexpr std::uint32_t kImageStart = 0x00000000;

// Most exchanges go through UdsClient over CanFlashUdsChannel. The
// exceptions are the probes and fire-and-forget writes whose reply legacy
// either tolerates or never inspects at all: UdsClient enforces the
// SID+0x40 convention and decodes NRCs, so those go through the channel --
// or, where legacy does not even validate the reply's arbitration id,
// through the transport -- directly. `channel` is the same
// CanFlashUdsChannel instance `uds` wraps.
// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

// Bench programming arm, legacy lines 657-805.
Status ConnectBench(Ctx& ctx)
{
    LogInfo(ctx, "Bench programming: accessing, please wait...");
    if (const Status slept = ctx.clock.Sleep(500ms, ctx.cancellation); !slept.has_value())
    {
        return slept;
    }

    // Lines 661-689.
    if (Result<bytes::Bytes> session =
            FatalQuery(ctx, bytes::Bytes{uds::kSidDiagnosticSessionControl, kSessionBench}, bytes::Bytes{kSessionBench},
                       kShortPolicy, "bench diagnostic session");
        !session.has_value())
    {
        return std::unexpected(session.error());
    }

    // Lines 691-768.
    if (const Status unlocked = DensoSecurityAccess(ctx); !unlocked.has_value())
    {
        return unlocked;
    }

    return DensoIso15765JumpToKernel(ctx, kSessionBenchJump, 50, kShortTimeout, kShortTimeout,
                                     /*discard_first_reply=*/true);
}

// Legacy connect_bootloader, lines 91-808. The legacy `is_serial_port_open()`
// guard (lines 99-103) has no equivalent: execute() below opens the transport
// through the port and propagates that Status, so an unopened bus fails
// earlier and with a typed error.
Status ConnectBootloader(Ctx& ctx, ICanFlashTransport& can)
{
    // OBK-already-active probe (lines 107-128). A match short-circuits the
    // whole sequence; a mismatch or an empty read falls through.
    Result<bool> obk_active = DensoObkIsActive(ctx, "OBK is active!");
    if (!obk_active.has_value())
    {
        return std::unexpected(obk_active.error());
    }
    if (*obk_active)
    {
        return {};
    }

    // Lines 131-281: four non-fatal identity queries.
    DensoEcuIdQuery(ctx);
    DensoVehicleInfoQueries(ctx);

    // Lines 283-341: the tolerant access-method probe and branch selector.
    Result<bool> in_car = DensoProgrammingBranchIsInCar(ctx, kProbeTimeout);
    if (!in_car.has_value())
    {
        return std::unexpected(in_car.error());
    }
    return *in_car ? DensoIso15765ConnectInCar(ctx, can, DensoInCarRun::kSh, kShortTimeout) : ConnectBench(ctx);
}

// Legacy read_memory, lines 815-1073. start_addr and length are overwritten
// with 0x8000 / 0x137F00 at lines 828-830 regardless of the caller's
// arguments; the plan carries exactly that region, so `region` reproduces it.
Result<bytes::Bytes> ReadMemory(Ctx& ctx, const SubaruDensoSh72531CanPlan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    // Lines 837-911: both setup exchanges are fatal on a mismatch and on an
    // absent reply, and both read with serial_read_timeout (lines 856, 893),
    // not the short timeout its 1N83M 1.5M sibling uses here.
    LogInfo(ctx, "Settting dump start & length");
    if (Result<bytes::Bytes> download =
            FatalQuery(ctx, DensoIso15765DownloadSetupPdu(region), bytes::Bytes{0x20, 0x01, 0x05}, kLongPolicy,
                       "dump start & length setup");
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (Result<bytes::Bytes> upload = FatalQuery(ctx, DensoIso15765UploadSetupPdu(region),
                                                 bytes::Bytes{0x20, 0x01, 0x01}, kLongPolicy, "dump upload setup");
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    return DensoIso15765ReadDump(ctx, region, family.lead_pad_len, family.tail_pad_len, progress,
                                 /*pace_pages=*/true);
}

// Legacy reflash_block, lines 1167-1350, called for block 1 only.
Status ReflashBlock(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    LogInfo(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, kImageStart, block, progress, kChecksumPolicy);
}

// Legacy write_memory, lines 1081-1160. block_modified is {0, 1, 0} over
// numblocks == 3, so exactly one block is erased and reflashed: fblocks[1],
// which is the plan's transfer region.
Status WriteMemory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    return DensoIso15765WriteBlock(ctx, block, 1, phases,
                                   [&](PhaseReporter& write) { return ReflashBlock(ctx, image, block, write); });
}

} // namespace

Result<Iso15765Config> SubaruDensoSh72531CanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruDensoSh72531Can); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruDensoSh72531CanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruDensoSh72531CanPlan>(plan.FamilyPlan());
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruDensoSh72531CanExecutor::Execute(const FlashPlan& plan,
                                                                    ICanFlashTransport& transport, IClock& clock,
                                                                    const ICancellationToken& cancellation,
                                                                    IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh72531Can); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh72531CanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDensoSh72531CanPlan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start("Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    LogInfo(ctx, "Connecting to ECU Denso SH72531 CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(ctx, transport); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (read)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, "Reading ROM from ECU, Denso SH72531 using CAN");

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

    // build_subaru_denso_sh72531_can_plan refuses TestWrite; the guard is
    // repeated here so a plan built another way cannot turn a dry run into a
    // real erase and write. Legacy threaded test_write all the way down to
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
        return Fail(ErrorKind::kUnsupported, "test_write is not supported by the Subaru Denso SH72531 CAN family");
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, "Writing ROM to ECU, Denso SH72531 using CAN");
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
