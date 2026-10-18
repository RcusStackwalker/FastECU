#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_executor.h"

#include <algorithm>
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
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below cites the line of
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_1n83m_4m_can_operation.cpp
// it was transcribed from, as of 20892df -- this branch's base, and the last
// commit before the file was deleted here. Master later reflowed one line in
// three of the four (S1117, #240), so resolve citations against 20892df.
//
// This family is the tolerant member of the Denso ISO-15765 bootloader dialect:
// seven of its response checks have their `return STATUS_ERROR` commented out
// (lines 305, 335, 369, 876, 883, 917, 924). Only four of those are a
// behavioural divergence -- the read_memory dump-setup checks at 876, 883,
// 917 and 924, where the 1N83M 1.5M and SH72531 siblings abort. At the three
// connect_bootloader probe sites (305, 335, 369) the siblings log and carry
// on too; they simply have no commented-out line there, so this family
// differs only in carrying its own record that the strictness was removed.
// It also reads twice after the bench kernel jump where the 1N83M 1.5M reads
// once (the SH72531 sibling reads twice as well), and spells its read
// timeouts as bare 200/500 literals rather than through the header's named
// constants. All of that is transcribed as found; each tolerated point is
// marked below.
namespace fastecu::flash
{
namespace
{

// Legacy's read timeouts. This family writes most of them as bare literals
// (200 at lines 677, 709, 754, 788, 790; 500 at lines 1056, 1409, 1452) where
// the header still declares serial_read_short_timeout = 200 and
// receive_timeout = 500 (header lines 43-49); the numbers, not the spellings,
// are the wire behaviour, and they are kept apart rather than flattened into
// one policy.
constexpr std::chrono::milliseconds kShortTimeout{200};   // serial_read_short_timeout, and the bare 200 literals
constexpr std::chrono::milliseconds kReceiveTimeout{500}; // receive_timeout, and the bare 500 literals
// The timeout the tolerant_probe exchanges in connect_bootloader read with:
// serial_read_short_timeout, as in two of its three siblings.
// Named rather than repeated at each call so the one line a reader has to
// compare across the four families is this one.
constexpr std::chrono::milliseconds kProbeTimeout = kShortTimeout;
constexpr std::chrono::milliseconds kExtraLongTimeout{3000}; // serial_read_extra_long_timeout
// Checksum verify reads twice and this family's two reads differ: the first
// uses the bare 500 literal (line 1322, i.e. receive_timeout) and the re-read
// after the ECU's 7F 31 78 pending answer uses
// serial_read_extra_long_timeout (line 1341). UdsClient substitutes
// pending_timeout for that second read, so the pair is spelled out here
// rather than left to the 3000 ms default -- the three sibling families read
// 500/500, 2000/2000 and 2000/2000 at this same exchange, so the default is
// not a shared number and each family pins its own.
constexpr uds::ExchangePolicy kChecksumPolicy{.read_timeout = kReceiveTimeout, .pending_timeout = kExtraLongTimeout};

// Positive-response service ids for the two SIDs this file inspects without
// going through UdsClient (which does the SID + 0x40 arithmetic itself).
constexpr bytes::Byte kRequestDownloadReply = uds::kSidRequestDownload + 0x40; // 0x74
constexpr bytes::Byte kRequestUploadReply = uds::kSidRequestUpload + 0x40;     // 0x75

// kFlashBlocksN83M_4MB[0].start -- reflash_block indexes the image as
// newdata[i + blockaddr - fdt->fblocks[0].start] (line 1241) and write_memory
// hands it &data_array[0] (line 1153), the whole encrypted ROM, so byte 0 of
// the plan image is this address.
constexpr std::uint32_t kImageStart = 0x08F9C000;

// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;

// read_memory's two dump-setup exchanges (lines 843-884 and 886-925). Unlike
// tolerant_probe this tolerates BOTH halves of the check.
//
// TOLERATED, lines 876 / 883 (the 0x34 RequestDownload setup) and 917 / 924
// (the 0x35 RequestUpload setup): all four `return STATUS_ERROR` statements
// are commented out, so neither a wrong reply nor an absent one stops the
// dump -- the early return is intentionally absent. This is the family's one
// genuine behavioural divergence from its 1N83M 1.5M and SH72531 siblings,
// which abort at all four. A transport-level failure is still propagated:
// legacy's read_serial_data has no error channel at all, so a broken bus has
// no legacy behaviour to preserve and must surface rather than be mistaken
// for an empty reply.
Status TolerantSetup(Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix)
{
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(kShortTimeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    // Legacy requires received.length() > 7, i.e. four bytes past the 4-byte
    // envelope, before it inspects the reply at all.
    if (!received->has_value() || received->value().size() < 4)
    {
        LogError(ctx, "No valid response from ECU");
        return {};
    }
    const bytes::Bytes& frame = **received;
    if (!std::equal(expected_prefix.begin(), expected_prefix.end(), frame.begin()))
    {
        LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, bytes::ToHex(frame)));
    }
    return {};
}

// Legacy connect_bootloader, lines 89-810. The legacy `is_serial_port_open()`
// guard (lines 95-101) has no equivalent: execute() below opens the transport
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

    // Lines 283-343: the tolerant access-method probe and branch selector.
    Result<bool> in_car = DensoProgrammingBranchIsInCar(ctx, kProbeTimeout);
    if (!in_car.has_value())
    {
        return std::unexpected(in_car.error());
    }
    return *in_car ? DensoIso15765ConnectInCar(ctx, can, DensoInCarRun::kN83m, kShortTimeout)
                   : DensoIso15765ConnectBench(ctx, /*discard_first_reply=*/true);
}

// Legacy read_memory, lines 821-1081. start_addr and length are overwritten
// with 0x08FAC000 / 0x003D3F00 at lines 834-836 regardless of the caller's
// arguments; the plan carries exactly that region, so `region` reproduces it.
Result<bytes::Bytes> ReadMemory(Ctx& ctx, const SubaruDenso1n83m_4mCanPlan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    // Lines 843-925: unlike every sibling family, BOTH setup exchanges
    // tolerate a wrong reply AND an absent one -- see tolerant_setup for the
    // four commented-out returns this preserves.
    LogInfo(ctx, "Settting dump start & length");
    if (const Status download = TolerantSetup(ctx, DensoIso15765DownloadSetupPdu(region),
                                              bytes::Bytes{kRequestDownloadReply, 0x20, 0x01, 0x05});
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (const Status upload = TolerantSetup(ctx, DensoIso15765UploadSetupPdu(region),
                                            bytes::Bytes{kRequestUploadReply, 0x20, 0x01, 0x01});
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    return DensoIso15765ReadDump(ctx, region, family.lead_pad_len, family.tail_pad_len, progress,
                                 /*pace_pages=*/false);
}

// Legacy reflash_block, lines 1180-1367, called for block 1 only.
Status ReflashBlock(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseReporter& progress)
{
    LogInfo(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, kImageStart, block, progress, kChecksumPolicy);
}

// Legacy write_memory, lines 1095-1173. block_modified is {0, 1, 0} over
// numblocks == 3, so exactly one block is erased and reflashed: fblocks[1],
// which is the plan's transfer region.
Status WriteMemory(Ctx& ctx, bytes::ByteView image, const MemoryRegion& block, PhaseSequence& phases)
{
    // Erase setup remains fatal (legacy lines 1421, 1427); only read setup is tolerant.
    return DensoIso15765WriteBlock(ctx, block, 1, phases,
                                   [&](PhaseReporter& write) { return ReflashBlock(ctx, image, block, write); });
}

} // namespace

Result<Iso15765Config> SubaruDenso1n83m_4mCanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruDenso1n83m4mCan); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruDenso1n83m4mCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& family = std::get<SubaruDenso1n83m_4mCanPlan>(plan.FamilyPlan());
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruDenso1n83m_4mCanExecutor::Execute(const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDenso1n83m4mCan); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDenso1n83m4mCanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDenso1n83m_4mCanPlan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start("Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Ctx ctx{cancellation, events, clock, uds_client, channel};

    LogInfo(ctx, "Connecting to ECU Denso 1N83M 4MB CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(ctx, transport); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (read)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, "Reading ROM from ECU, Denso 1N83M 4MB using CAN");

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

    // build_subaru_denso_1n83m_4m_can_plan refuses TestWrite; the guard is
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
        return Fail(ErrorKind::kUnsupported, "test_write is not supported by the Subaru Denso 1N83M 4M CAN family");
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, "Writing ROM to ECU, Denso 1N83M 4MB using CAN");
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
