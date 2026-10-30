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
#include "src/backend/flash/ecu/bootloader_operation.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_1_5m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_1n83m_4m_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72531_can_plan.h"
#include "src/backend/flash/ecu/subaru_denso_sh72543_can_diesel_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below was transcribed from the legacy
// src/platform/desktop/common/flash/legacy/ecu/flash_ecu_subaru_denso_*_can*_operation.cpp
// files as of 20892df, the last commit before they were deleted. The flow
// follows SH72531's; a family's deviations sit on its profile.
namespace fastecu::flash
{
namespace
{

// The five-field context this package shares; see uds_client_exchange_common.h.
using Ctx = CanExecutorContext;
using Profile = DensoIso15765BootloaderProfile;

// SH72543 diesel's ECU ID query: a ReadDataByIdentifier of DID F182 read at
// serial_read_timeout. It needs a two-byte reply check (`F1 82`) where the
// shared NonFatalQuery checks one subfunction byte. Same shape otherwise:
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
    // `62 F1 82` -- and although its remove(5, len-5) truncation is commented
    // out, the loop that builds the logged string runs `for (int i = 0; i < 5;
    // i++)`, so exactly five bytes are logged. Take the same five. Fewer are
    // taken only if the reply is short, where legacy would index past the end
    // instead.
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

// The legacy `is_serial_port_open()` guard has no equivalent: Execute() opens
// the transport through the port and propagates that Status, so an unopened bus
// fails earlier and with a typed error.
Status ConnectBootloader(const Ctx& ctx, ICanFlashTransport& can, const Profile& profile)
{
    // OBK-already-active probe. A match short-circuits the whole sequence; a
    // mismatch or an empty read falls through.
    Result<bool> obk_active = DensoObkIsActive(ctx, profile.obk_active_log);
    if (!obk_active.has_value())
    {
        return std::unexpected(obk_active.error());
    }
    if (*obk_active)
    {
        return {};
    }

    // Four non-fatal identity queries.
    IdentityQuery(ctx, profile.identity_query);
    DensoVehicleInfoQueries(ctx);

    // The tolerant access-method probe and branch selector.
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
// `return STATUS_ERROR` statements at commented out): a wrong or absent (or
// too-short) reply is logged and stepped over. Legacy requires
// received.length() > 7, i.e. four bytes past the 4-byte envelope, before it
// inspects the reply at all. A transport-level failure is still propagated:
// legacy's read_serial_data has no error channel at all, so a broken bus has no
// legacy behaviour to preserve and must surface rather than be mistaken for an
// empty reply.
Status TolerantSetup(const Ctx& ctx, bytes::ByteView pdu, bytes::ByteView expected_prefix,
                     std::chrono::milliseconds timeout)
{
    return ctx.channel.Send(pdu, ctx.cancellation)
        .and_then([&] { return ctx.channel.Receive(timeout, ctx.cancellation); })
        .transform(
            [&](const std::optional<bytes::Bytes>& received)
            {
                if (!received.has_value() || received->size() < 4)
                {
                    LogError(ctx, "No valid response from ECU");
                    return;
                }
                if (const bytes::Bytes& frame = *received;
                    !std::equal(expected_prefix.begin(), expected_prefix.end(), frame.begin()))
                {
                    LogError(ctx, std::format("{}{}", kDensoIso15765RejectionPrefix, bytes::ToHex(frame)));
                }
            });
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

// Legacy overwrites start_addr and length with the family's literal region
// regardless of the caller's arguments (SH72543 diesel has that overwrite
// commented out and passes the same region as arguments); the plan carries
// exactly that region, so `region` reproduces it.
template <typename Plan>
Result<bytes::Bytes> ReadMemory(const Ctx& ctx, const Profile& profile, const Plan& family, const MemoryRegion& region,
                                PhaseReporter& progress)
{
    LogInfo(ctx, profile.region_log == DensoRegionLog::kStartAndLength ? StartAndLengthLine(region)
                                                                       : "Settting dump start & length");
    return DumpSetup(ctx, profile, DensoIso15765DownloadSetupPdu(region),
                     static_cast<bytes::Byte>(uds::kSidRequestDownload + 0x40), 0x05, "dump start & length setup")
        .and_then(
            [&]
            {
                return DumpSetup(ctx, profile, DensoIso15765UploadSetupPdu(region),
                                 static_cast<bytes::Byte>(uds::kSidRequestUpload + 0x40), 0x01, "dump upload setup");
            })
        .and_then(
            [&]
            {
                return DensoIso15765ReadDump(ctx, region, family.lead_pad_len, family.tail_pad_len, progress,
                                             profile.pace_pages);
            });
}

// Called for the profile's one block.
Status ReflashBlock(const Ctx& ctx, const Profile& profile, bytes::ByteView image, const MemoryRegion& block,
                    PhaseReporter& progress)
{
    LogInfo(ctx, profile.region_log == DensoRegionLog::kStartAndLength
                     ? StartAndLengthLine(block)
                     : std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    return DensoIso15765WriteImageAndVerifyChecksum(ctx, image, profile.image_start, block, progress,
                                                    profile.checksum_policy);
}

// block_modified marks exactly one block -- the plan's transfer region -- so
// one block is erased and reflashed. The log lines carry the profile's block
// index: 1 of a three-block table on the N83M and SH72531 families, 0 of
// SH72543 diesel's one-block table.
} // namespace

template <typename Plan> Result<Iso15765Config> DensoIso15765BootloaderExecutor<Plan>::Setup(const Plan& family) const
{
    return Iso15765ConfigFrom(family);
}

template <typename Plan>
Result<FlashExecutionResult>
DensoIso15765BootloaderExecutor<Plan>::Run(const Plan& family, const FlashPlan& plan, ICanFlashTransport& transport,
                                           IClock& clock, const ICancellationToken& cancellation, IEventSink& events)
{
    static constexpr const Profile& kProfile = DensoIso15765BootloaderProfileOf<Plan>::kValue;

    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    const Ctx ctx{cancellation, events, clock, uds_client, channel};
    return RunBootloaderOperation(
        plan, kProfile.unit, ctx, [&](const Ctx& c) { return ConnectBootloader(c, transport, kProfile); },
        [&](const Ctx& c, PhaseReporter& progress)
        { return ReadMemory(c, kProfile, family, plan.TransferRegion(), progress); },
        [&](const Ctx& c)
        {
            LogInfo(c, std::format("Blocks to flash: {},  (total: 1)", kProfile.write_block_index));
            LogInfo(c, "--- Erasing ECU flash memory ---");
            return DensoIso15765Erase(c, plan.TransferRegion());
        },
        [&](const Ctx& c, bytes::ByteView image, PhaseReporter& progress)
        {
            LogInfo(c, "--- Start writing ROM file to ECU flash memory ---");
            return ReflashBlock(c, kProfile, image, plan.TransferRegion(), progress)
                .transform([&] { LogInfo(c, std::format("Block {} reflash complete.", kProfile.write_block_index)); });
        });
}

template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_1_5mCanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDenso1n83m_4mCanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDensoSh72531CanPlan>;
template class DensoIso15765BootloaderExecutor<SubaruDensoSh72543CanDieselPlan>;

} // namespace fastecu::flash
