#include "src/backend/flash/ecu/denso_iso15765_bootloader_executor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below was transcribed from the legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_*_can*_operation.cpp
// files as of 20892df, the last commit before they were deleted. Line
// citations for a family's deviations sit on its profile; the ones here are
// SH72531's, which the flow follows.
namespace fastecu::flash
{
namespace
{

// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;
using Profile = DensoIso15765BootloaderProfile;

template <typename Plan> struct FamilyOf;

template <> struct FamilyOf<SubaruDenso1n83m_1_5mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDenso1n83m15mCan;
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDenso1n83m15mCanPlan(plan);
    }
};

template <> struct FamilyOf<SubaruDenso1n83m_4mCanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDenso1n83m4mCan;
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDenso1n83m4mCanPlan(plan);
    }
};

template <> struct FamilyOf<SubaruDensoSh72531CanPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh72531Can;
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh72531CanPlan(plan);
    }
};

template <> struct FamilyOf<SubaruDensoSh72543CanDieselPlan>
{
    static constexpr FlashFamily kFamily = FlashFamily::kSubaruDensoSh72543CanDiesel;
    static Status Validate(const FlashPlan& plan)
    {
        return ValidateSubaruDensoSh72543CanDieselPlan(plan);
    }
};

template <typename Plan> Status CheckPlan(const FlashPlan& plan)
{
    if (const Status match = CheckFamily(plan, FamilyOf<Plan>::kFamily); !match.has_value())
    {
        return match;
    }
    return FamilyOf<Plan>::Validate(plan);
}

// SH72543 diesel's ECU ID query: a ReadDataByIdentifier of DID F182 read at
// serial_read_timeout. It needs a two-byte reply check (`F1 82`) where the
// shared non_fatal_query checks one subfunction byte. Same shape otherwise:
// log on a match, log and continue on anything else.
void DidF182Query(const Ctx& ctx)
{
    constexpr bytes::Byte kDidEcuIdHigh = 0xF1;
    constexpr bytes::Byte kDidEcuIdLow = 0x82;
    constexpr uds::ExchangePolicy kDidPolicy{.read_timeout = std::chrono::milliseconds{2000}};
    constexpr std::string_view kLabel = "ECU ID";
    const bytes::Bytes expected_did{kDidEcuIdHigh, kDidEcuIdLow};

    Result<bytes::Bytes> reply = ctx.uds.Request(
        bytes::Bytes{uds::kSidReadDataByIdentifier, kDidEcuIdHigh, kDidEcuIdLow}, kDidPolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, reply.error().detail));
        return;
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.size() < expected_did.size() || !std::equal(expected_did.begin(), expected_did.end(), payload.begin()))
    {
        LogError(ctx, std::format("{}unexpected {} response", kDensoIso15765RejectionPrefix, kLabel));
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
    LogInfo(ctx, std::format("{}: {}", kLabel, bytes::ToHex(rest.subspan(0, std::min(kEcuIdBytes, rest.size())))));
}

void IdentityQuery(const Ctx& ctx, DensoIdentityQuery query)
{
    if (query == DensoIdentityQuery::kDidF182)
    {
        LogInfo(ctx, "Requesting ECU ID");
        DidF182Query(ctx);
        return;
    }
    DensoEcuIdQuery(ctx);
}

// Legacy connect_bootloader, lines 91-808. The legacy `is_serial_port_open()`
// guard has no equivalent: Execute() opens the transport through the port and
// propagates that Status, so an unopened bus fails earlier and with a typed
// error.
Status ConnectBootloader(const Ctx& ctx, ICanFlashTransport& can, const Profile& profile)
{
    // OBK-already-active probe (lines 107-128). A match short-circuits the
    // whole sequence; a mismatch or an empty read falls through.
    Result<bool> obk_active = DensoObkIsActive(ctx, profile.obk_active_log);
    if (!obk_active.has_value())
    {
        return std::unexpected(obk_active.error());
    }
    if (*obk_active)
    {
        return {};
    }

    // Lines 131-281: four non-fatal identity queries.
    IdentityQuery(ctx, profile.identity_query);
    DensoVehicleInfoQueries(ctx);

    // Lines 283-341: the tolerant access-method probe and branch selector.
    Result<bool> in_car = DensoProgrammingBranchIsInCar(ctx, profile.probe_timeout);
    if (!in_car.has_value())
    {
        return std::unexpected(in_car.error());
    }
    return *in_car ? DensoIso15765ConnectInCar(ctx, can, profile.in_car_run, profile.in_car_timeout)
                   : DensoIso15765ConnectBench(ctx, profile.bench);
}

std::string StartAndLengthLine(const MemoryRegion& region)
{
    // QString::number(x, 16) is lowercase and unpadded.
    return std::format("Setting flash start: 0x{:x} and length: 0x{:x}", region.start, region.length);
}

// A tolerant-reply dump setup (1N83M 4M, whose legacy read_memory has all four
// `return STATUS_ERROR` statements at lines 876 / 883 / 917 / 924 commented
// out): a wrong or absent (or too-short) reply is logged and stepped over.
// Legacy requires received.length() > 7, i.e. four bytes past the 4-byte
// envelope, before it inspects the reply at all. A transport-level failure is
// still propagated: legacy's read_serial_data has no error channel at all, so a
// broken bus has no legacy behaviour to preserve and must surface rather than
// be mistaken for an empty reply.
Status TolerantSetup(const Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                     std::chrono::milliseconds timeout)
{
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    Result<std::optional<bytes::Bytes>> received = ctx.channel.Receive(timeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value() || received->value().size() < 4)
    {
        LogError(ctx, "No valid response from ECU");
        return {};
    }
    if (const bytes::Bytes& frame = **received;
        !std::equal(expected_prefix.begin(), expected_prefix.end(), frame.begin()))
    {
        LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, bytes::ToHex(frame)));
    }
    return {};
}

// One of read_memory's two dump-setup exchanges, expecting `20 01 <tail>` after
// the reply's service id.
Status DumpSetup(const Ctx& ctx, const Profile& profile, bytes::ByteView pdu, bytes::Byte reply_service,
                 bytes::Byte tail, std::string_view subject)
{
    if (profile.read_setup_reply == DensoReadSetupReply::kTolerantReply)
    {
        return TolerantSetup(ctx, pdu, bytes::Bytes{reply_service, 0x20, 0x01, tail},
                             profile.read_setup_policy.read_timeout);
    }
    if (Result<bytes::Bytes> reply =
            FatalQuery(ctx, pdu, bytes::Bytes{0x20, 0x01, tail}, profile.read_setup_policy, subject);
        !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return {};
}

// Legacy read_memory, lines 815-1073. Legacy overwrites start_addr and length
// with the family's literal region regardless of the caller's arguments
// (SH72543 diesel has that overwrite commented out and passes the same region
// as arguments); the plan carries exactly that region, so `region`
// reproduces it.
template <typename Plan>
Result<bytes::Bytes> ReadMemory(const Ctx& ctx, const Profile& profile, const Plan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    LogInfo(ctx, profile.region_log == DensoRegionLog::kStartAndLength ? StartAndLengthLine(region)
                                                                       : "Settting dump start & length");
    if (const Status download =
            DumpSetup(ctx, profile, DensoIso15765DownloadSetupPdu(region),
                      static_cast<bytes::Byte>(uds::kSidRequestDownload + 0x40), 0x05, "dump start & length setup");
        !download.has_value())
    {
        return std::unexpected(download.error());
    }
    if (const Status upload =
            DumpSetup(ctx, profile, DensoIso15765UploadSetupPdu(region),
                      static_cast<bytes::Byte>(uds::kSidRequestUpload + 0x40), 0x01, "dump upload setup");
        !upload.has_value())
    {
        return std::unexpected(upload.error());
    }

    return DensoIso15765ReadDump(ctx, region, family.lead_pad_len, family.tail_pad_len, progress, profile.pace_pages);
}

// Legacy reflash_block, lines 1167-1350, called for the profile's one block.
Status ReflashBlock(const Ctx& ctx, const Profile& profile, bytes::ByteView image, const MemoryRegion& block,
                    PhaseReporter& progress)
{
    LogInfo(ctx, profile.region_log == DensoRegionLog::kStartAndLength
                     ? StartAndLengthLine(block)
                     : std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, profile.image_start, block, progress,
                                                    profile.checksum_policy);
}

// Legacy write_memory, lines 1081-1160. block_modified marks exactly one block
// -- the plan's transfer region -- so one block is erased and reflashed.
Status WriteMemory(const Ctx& ctx, const Profile& profile, bytes::ByteView image, const MemoryRegion& block,
                   PhaseSequence& phases)
{
    return DensoIso15765WriteBlock(ctx, block, profile.write_block_index, phases,
                                   [&ctx, &profile, image, &block](PhaseReporter& write)
                                   { return ReflashBlock(ctx, profile, image, block, write); });
}

} // namespace

template <typename Plan>
Result<Iso15765Config> DensoIso15765BootloaderExecutor<Plan>::TransportSetup(const FlashPlan& plan) const
{
    if (const Status valid = CheckPlan<Plan>(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return Iso15765ConfigFrom(std::get<Plan>(plan.FamilyPlan()));
}

template <typename Plan>
Result<FlashExecutionResult>
DensoIso15765BootloaderExecutor<Plan>::Execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                               const ICancellationToken& cancellation, IEventSink& events)
{
    constexpr const Profile& kProfile = DensoIso15765BootloaderProfileOf<Plan>::kValue;
    if (const Status valid = CheckPlan<Plan>(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<Plan>(plan.FamilyPlan());

    const bool read = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read ? 2 : 3);
    PhaseReporter connect = phases.Start("Connect", 1);

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    const Ctx ctx{cancellation, events, clock, uds_client, channel};

    LogInfo(ctx, std::format("Connecting to ECU {} CAN bootloader, please wait...", kProfile.log_name));
    if (const Status connected = ConnectBootloader(ctx, transport, kProfile); !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    connect.Complete();

    if (read)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, std::format("Reading ROM from ECU, {} using CAN", kProfile.log_name));

        PhaseReporter read_phase = phases.Start("Read ROM", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = ReadMemory(ctx, kProfile, family, plan.TransferRegion(), read_phase);
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

    // Each family's plan builder refuses TestWrite; the guard is repeated here
    // so a plan built another way cannot turn a dry run into a real erase and
    // write. Legacy threaded test_write all the way down to reflash_block and
    // then never consulted it.
    //
    // It cannot fire as the code stands: the plan validation at the top of
    // Execute() rejects TestWrite before any I/O, and the Read branch has
    // already returned, so FlashOperation has no third value left to reach
    // here. The bootloader-dialect factoring pass reviewed it across all four
    // families and kept it: it costs nothing at runtime and is the last
    // thing between a non-Write operation and a real erase-and-write of an
    // ECU should that entry validation ever be relaxed or the enum gain a
    // value. Do not delete it as "dead code".
    if (plan.Operation() != FlashOperation::kWrite)
    {
        return Fail(ErrorKind::kUnsupported,
                    std::format("test_write is not supported by the {} family", kProfile.family_name));
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, std::format("Writing ROM to ECU, {} using CAN", kProfile.log_name));
    if (const Status written = WriteMemory(ctx, kProfile, plan.ImageOrEmpty(), plan.TransferRegion(), phases);
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    return FlashExecutionResult{
        .operation = plan.Operation(),
        .read_bytes = std::nullopt,
    };
}

template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_1_5mCanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_4mCanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDensoSh72531CanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDensoSh72543CanDieselPlan>;

} // namespace fastecu::flash
