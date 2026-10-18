#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_executor.h"

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
#include <chrono>
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_1_5m_can_operation.cpp
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
// they are this family's own wire timing (header lines 44-47). The third,
// serial_read_timeout (2000), is now read only by the shared connect and page
// read in denso_iso15765_can_common.
constexpr std::chrono::milliseconds kShortTimeout{200};   // serial_read_short_timeout
constexpr std::chrono::milliseconds kReceiveTimeout{500}; // receive_timeout
constexpr uds::ExchangePolicy kShortPolicy{.read_timeout = kShortTimeout};
// The timeout the tolerant_probe exchanges in connect_bootloader read with:
// serial_read_short_timeout, as in two of its three siblings.
// Named rather than repeated at each call so the one line a reader has to
// compare across the four families is this one.
constexpr std::chrono::milliseconds kProbeTimeout = kShortTimeout;
// Checksum verify reads twice and this family uses receive_timeout for both:
// the first read at line 1308 and, after the ECU's 7F 31 78 pending answer,
// the re-read at line 1329. UdsClient substitutes pending_timeout for that
// second read, whose 3000 ms default is not this family's number, so the
// policy pins it to 500 as well.
constexpr uds::ExchangePolicy kChecksumPolicy{.read_timeout = kReceiveTimeout, .pending_timeout = kReceiveTimeout};

// Session ids in ISO 14229-1's 0x40-0x5F vehicle-manufacturer-specific band;
// legacy uses its own values here rather than the standard subfunctions.
constexpr bytes::Byte kSessionBench = 0x43;     // bench programming session
constexpr bytes::Byte kSessionBenchJump = 0x42; // bench jump to on-board kernel
// kFlashBlocksN83M_1_5MB[0].start -- reflash_block indexes the image as
// newdata[i + blockaddr - fdt->fblocks[0].start] (line 1225), so byte 0 of
// the plan image is this address.
constexpr std::uint32_t kImageStart = 0x08F9C000;

// Most exchanges go through UdsClient over CanFlashUdsChannel. The
// exceptions are the probes and fire-and-forget writes whose reply legacy
// either tolerates or never inspects at all: UdsClient enforces the
// SID+0x40 convention and decodes NRCs, so those go through the channel --
// or, where legacy does not even validate the reply's arbitration id,
// through the transport -- directly. `channel` is the same
// CanFlashUdsChannel instance `uds` wraps.
// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

// Bench programming arm, legacy lines 657-803.
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
                                     /*discard_first_reply=*/false);
}

// Legacy connect_bootloader, lines 89-806. The legacy `is_serial_port_open()`
// guard (lines 97-101) has no equivalent: execute() below opens the transport
// through the port and propagates that Status, so an unopened bus fails
// earlier and with a typed error.
Status ConnectBootloader(Ctx& ctx, ICanFlashTransport& can)
{
    // OBK-already-active probe (lines 105-127). A match short-circuits the
    // whole sequence; a mismatch or an empty read falls through.
    Result<bool> obk_active = DensoObkIsActive(ctx, "OBK is active");
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
    return *in_car ? DensoIso15765ConnectInCar(ctx, can, DensoInCarRun::kN83m, kShortTimeout) : ConnectBench(ctx);
}

// Legacy read_memory, lines 813-1074. start_addr and length are overwritten
// with 0x08FAC000 / 0x00173F00 at lines 826-828 regardless of the caller's
// arguments; the plan carries exactly that region, so `region` reproduces it.
Result<bytes::Bytes> ReadMemory(Ctx& ctx, const SubaruDenso1n83m_1_5mCanPlan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    // Lines 835-917: both setup exchanges are fatal on a mismatch and on an
    // absent reply.
    LogInfo(ctx, "Settting dump start & length");
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
                                 /*pace_pages=*/false);
}

// Legacy reflash_block, lines 1167-1357, called for block 1 only.
Status ReflashBlock(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    LogInfo(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, kImageStart, block, progress, kChecksumPolicy);
}

// Legacy write_memory, lines 1082-1160. block_modified is {0, 1, 0} over
// numblocks == 3, so exactly one block is erased and reflashed: fblocks[1],
// which is the plan's transfer region.
Status WriteMemory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    return DensoIso15765WriteBlock(ctx, block, 1, phases,
                                   [&](PhaseReporter& write) { return ReflashBlock(ctx, image, block, write); });
}

} // namespace

Result<Iso15765Config> SubaruDenso1n83m_1_5mCanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruDenso1n83m15mCan); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruDenso1n83m15mCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruDenso1n83m_1_5mCanPlan>(plan.FamilyPlan());
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruDenso1n83m_1_5mCanExecutor::Execute(const FlashPlan& plan,
                                                                       ICanFlashTransport& transport, IClock& clock,
                                                                       const ICancellationToken& cancellation,
                                                                       IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDenso1n83m15mCan); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDenso1n83m15mCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDenso1n83m_1_5mCanPlan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start("Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    // Legacy names the 4MB sibling in this line and in the two "using CAN"
    // lines below (20892df lines 62, 70 and 76) -- an upstream copy-paste that
    // the 4M sibling carries legitimately. Corrected here rather than
    // transcribed: this executor's own error strings already say 1.5M, so
    // leaving these would make a flash log unattributable to the executor that
    // produced it. A deliberate divergence from the legacy text, and the only
    // one in this file that is purely cosmetic.
    LogInfo(ctx, "Connecting to ECU Denso 1N83M 1.5MB CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(ctx, transport); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (read)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, "Reading ROM from ECU, Denso 1N83M 1.5MB using CAN");

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

    // build_subaru_denso_1n83m_1_5m_can_plan refuses TestWrite; the guard is
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
        return Fail(ErrorKind::kUnsupported, "test_write is not supported by the Subaru Denso 1N83M 1.5M CAN family");
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, "Writing ROM to ECU, Denso 1N83M 1.5MB using CAN");
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
