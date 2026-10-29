#include "src/backend/flash/ecu/denso_beef_can_ecu_execution.h"

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
#include "src/backend/flash/ecu/beef_can_kernel.h"
#include "src/backend/flash/ecu/beef_can_session.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below is transcribed from revision 59f4e442 of
// src/platform/desktop/common/flash/legacy/ecu/
// flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp: connect_bootloader
// (113-510), upload_kernel (517-782), read_mem (789-936), write_mem
// (943-1057), CRC/init/reflash/flash (1064-1689), security/crypto
// (1696-1790), and request_kernel_id (1797-1829).
// ISO-15765 is only the envelope transport. UDS is used for strict
// positive-response exchanges; tolerant identity/session queries and every
// proprietary BEEF exchange remain explicit channel requests.

namespace fastecu::flash
{
namespace
{

constexpr std::chrono::milliseconds kExtraShortTimeout{50};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kKernelStartDelay{100};
constexpr std::chrono::milliseconds kKernelStartReadTimeout{10};

constexpr uds::ExchangePolicy kStrictPolicy{
    .pre_read_delay = kExtraShortTimeout,
    .read_timeout = kReadTimeout,
    .pending_timeout = kExtraLongTimeout,
    .max_pending_repeats = 10,
};
constexpr uds::ExchangePolicy kKernelStartPolicy{
    .pre_read_delay = kExtraShortTimeout,
    .read_timeout = kKernelStartReadTimeout,
    .pending_timeout = kExtraLongTimeout,
    .max_pending_repeats = 10,
};

struct Context
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    ICanFlashTransport& transport;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
    BeefCanSession session;
    BeefCanKernel kernel;
};

Result<bytes::Bytes> StrictRequest(Context& context, bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, policy, context.cancellation);
    if (!reply.has_value())
    {
        context.session.LogFailure(reply.error());
        return std::unexpected(reply.error());
    }
    // A security-access reply with no service payload is refused.
    if (!pdu.empty() && pdu.front() == uds::kSidSecurityAccess && reply->size() <= 1)
    {
        const Error shorter{ErrorKind::kBadResponse, "security access reply is too short"};
        context.session.LogFailure(shorter);
        return std::unexpected(shorter);
    }
    return reply;
}

Status StrictPayload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label,
                     const uds::ExchangePolicy& policy = kStrictPolicy)
{
    Result<bytes::Bytes> reply = StrictRequest(context, pdu, policy);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.size() < expected.size() || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        const Error mismatch{ErrorKind::kBadResponse, std::format("unexpected response during {}", label)};
        context.session.LogFailure(mismatch);
        return std::unexpected(mismatch);
    }
    return {};
}

Status StrictService(Context& context, bytes::ByteView pdu, bytes::Byte expected_service, std::string_view label,
                     const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> reply = StrictRequest(context, pdu, policy);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (reply->empty() || reply->front() != expected_service)
    {
        const Error mismatch{ErrorKind::kBadResponse, std::format("unexpected response during {}", label)};
        context.session.LogFailure(mismatch);
        return std::unexpected(mismatch);
    }
    return {};
}

template <typename Family> struct EcuFamilyTraits;

template <> struct EcuFamilyTraits<SubaruDensoSh7058CanDieselPlan>
{
    static constexpr std::string_view kUnit = "Subaru 07+ Diesel 32-bit";
    static constexpr const DensoBeefCanProfile& kProfile = kSubaruDensoSh7058CanDieselBeefProfile;
    static constexpr BeefKernelTransferPolicy kTransferPolicy{kKernelStartPolicy, std::chrono::milliseconds{500}};

    static Result<bytes::Bytes> SecurityKey(Context&, bytes::ByteView seed)
    {
        if (seed.size() != 4)
        {
            return Fail(ErrorKind::kBadResponse, "diesel security seed must contain four bytes");
        }
        return DensoSeedKey(seed);
    }
};

// Logs the ECU's identity; a read prefixes the CAL ID to the image name.
Status RequestIdentity(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    if (Status ecu_id = context.session.RequestEcuId(read_operation, rom_id); !ecu_id.has_value())
    {
        return ecu_id;
    }
    LogInfo(context, "Requesting VIN");
    Result<std::optional<std::string>> vin =
        context.session.IdentityString(bytes::Bytes{uds::kSidVehicleInfoRequest, 0x02}, 0x49, 0x02, "VIN");
    if (!vin.has_value())
    {
        return std::unexpected(vin.error());
    }
    LogInfo(context, "Requesting CAL ID");
    Result<std::optional<std::string>> cal_id = context.session.IdentityString(
        bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, 0x49, 0x04, "CAL ID");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (read_operation && cal_id->has_value())
    {
        rom_id = **cal_id + "_" + rom_id.value_or(std::string{});
    }
    return context.session.RequestCvn();
}

Status UploadKernel(Context& context, const KernelImage& kernel, const BeefKernelTransferPolicy& policy)
{
    if (const Status uploaded = context.kernel.TransferPayload(kernel, policy); !uploaded.has_value())
    {
        return uploaded;
    }
    const std::size_t padded_size = ((kernel.bytes.size() + 127U) / 128U) * 128U;
    LogDebug(context, std::format("Data bytes sent: 0x{}", padded_size));

    LogInfo(context, "Kernel uploaded, starting...");
    if (const Status exited = StrictPayload(context, bytes::Bytes{uds::kSidRequestTransferExit}, {},
                                            "kernel transfer exit", kKernelStartPolicy);
        !exited.has_value())
    {
        return exited;
    }
    if (const Status delay = context.clock.Sleep(kKernelStartDelay, context.cancellation); !delay.has_value())
    {
        return delay;
    }
    if (const Status started = StrictService(context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02},
                                             0x71, "kernel start", kKernelStartPolicy);
        !started.has_value())
    {
        return started;
    }

    LogInfo(context, "Requesting kernel ID...");
    return context.session.StartedKernelId();
}

template <typename Family>
Result<FlashExecutionResult> RunEcuExecution(const Family& family, const FlashPlan& plan, ICanFlashTransport& transport,
                                             IClock& clock, const ICancellationToken& cancellation, IEventSink& events)
{
    using Traits = EcuFamilyTraits<Family>;
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{cancellation,
                    events,
                    clock,
                    transport,
                    uds_client,
                    channel,
                    BeefCanSession(transport, clock, cancellation, events, channel, Traits::kProfile),
                    BeefCanKernel(transport, clock, cancellation, events, channel, Traits::kProfile)};

    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(context.events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    std::optional<std::string> rom_id;
    LogInfo(context, std::format("Connecting to {} CAN bootloader, please wait...", Traits::kUnit));
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = context.session.RunningKernelId();
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        LogInfo(context, std::format("Kernel ID: {}", **kernel_id));
    }
    else
    {
        LogError(context, "No valid response from ECU");
        LogInfo(context, "No response from kernel, initialising ECU...");
        LogInfo(context, "Initializing connection...");
        if (const Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
        {
            return std::unexpected(identity.error());
        }
        auto sessions = context.session.RequestBootloaderSessions();
        if (!sessions.has_value())
        {
            return std::unexpected(sessions.error());
        }
        if (const Status unlocked = UnlockBeefSecurity(context, Traits::SecurityKey, kStrictPolicy);
            !unlocked.has_value())
        {
            return std::unexpected(unlocked.error());
        }
        if (const Status entered =
                EnterBeefProgrammingSession(context, sessions->mode_03, sessions->mode_43, kStrictPolicy);
            !entered.has_value())
        {
            return std::unexpected(entered.error());
        }
        if (plan.Kernel().has_value())
        {
            context.events.Notice("Preparing, please wait...");
            LogInfo(context, std::format("Initializing {} CAN kernel upload, please wait...", Traits::kUnit));
            if (const Status uploaded = UploadKernel(context, *plan.Kernel(), Traits::kTransferPolicy);
                !uploaded.has_value())
            {
                return std::unexpected(uploaded.error());
            }
        }
    }
    kernel_phase.Complete();

    if (read_operation)
    {
        context.events.Notice("Reading ROM, please wait...");
        LogInfo(context, std::format("Reading ROM from {} using CAN", Traits::kUnit));
        PhaseReporter read_phase = phases.Start("Read", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = context.kernel.Read(plan.TransferRegion(), read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead, .read_bytes = std::move(*rom), .rom_id = std::move(rom_id)};
    }

    context.events.Notice("Writing ROM, please wait...");
    LogInfo(context, std::format("Writing ROM to {} using CAN", Traits::kUnit));
    PhaseReporter compare_phase = phases.Start("Compare", static_cast<int>(plan.EraseRegions().size()));
    if (const Status written = context.kernel.Write(plan, phases, compare_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    PhaseReporter complete = phases.Start("Complete", 1);
    complete.Complete();
    return FlashExecutionResult{.operation = plan.Operation()};
}

} // namespace

Result<FlashExecutionResult> RunDensoBeefCanEcu(const SubaruDensoSh7058CanDieselPlan& family, const FlashPlan& plan,
                                                ICanFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events)
{
    return RunEcuExecution(family, plan, transport, clock, cancellation, events);
}

} // namespace fastecu::flash
