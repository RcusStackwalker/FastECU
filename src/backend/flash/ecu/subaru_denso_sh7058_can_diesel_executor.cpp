#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/beef_can_session.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_plan.h"
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

using bytes::ComposeBe;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kExtraShortTimeout{50};
constexpr std::chrono::milliseconds kMediumTimeout{500};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kKernelStartDelay{100};
constexpr std::chrono::milliseconds kKernelStartReadTimeout{10};
constexpr std::chrono::milliseconds kKernelTransferTimeout = kMediumTimeout;

constexpr std::size_t kKernelUploadBlockSize = 128;

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

Result<bytes::Bytes> SecurityKey(Context&, bytes::ByteView seed)
{
    if (seed.size() != 4)
    {
        return Fail(ErrorKind::kBadResponse, "diesel security seed must contain four bytes");
    }
    return DensoSeedKey(seed);
}

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

Status UnlockSecurity(Context& context)
{
    LogInfo(context, "Requesting seed");
    Result<bytes::Bytes> seed_reply =
        StrictRequest(context, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, kStrictPolicy);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView seed_payload = uds::Payload(*seed_reply);
    if (seed_payload.size() < 5 || seed_payload[0] != uds::kSecurityAccessRequestSeed)
    {
        LogError(context, "Wrong response from ECU: Not a valid answer");
        return Fail(ErrorKind::kBadResponse, "invalid diesel seed response");
    }
    LogInfo(context, "Seed request ok");
    Result<bytes::Bytes> key = SecurityKey(context, seed_payload.subspan(1, 4));
    if (!key.has_value())
    {
        return std::unexpected(key.error());
    }
    LogInfo(context, "Sending seed key");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key->begin(), key->end());
    if (const Status accepted = StrictPayload(context, key_request, bytes::Bytes{0x02}, "seed key");
        !accepted.has_value())
    {
        return accepted;
    }
    LogInfo(context, "Seed key ok");
    return {};
}

Status ConnectBootloader(Context& context, bool read_operation, bool& kernel_alive, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442:113-510.
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = context.session.RunningKernelId();
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        kernel_alive = true;
        return {};
    }
    LogError(context, "No valid response from ECU");
    LogInfo(context, "No response from kernel, initialising ECU...");
    LogInfo(context, "Initializing connection...");
    if (Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
    {
        return identity;
    }

    return ConnectBeefProgrammingSession(context, UnlockSecurity, kStrictPolicy);
}

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    // upload_kernel(), revision 59f4e442:517-782. The unusual <= loop is
    // intentional: after every 128-byte payload block the oracle emits one
    // final address-only 0xB6 request.
    const Result<bytes::Bytes> encrypted = BeefKernelUploadPayload(kernel.bytes, kKernelUploadBlockSize, "diesel");
    if (!encrypted.has_value())
    {
        return std::unexpected(encrypted.error());
    }

    LogDebug(context, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    LogInfo(context, "Initialize kernel upload");
    if (const Status initialized = StrictPayload(context, BeefKernelDownloadRequest(kernel.load_address, *encrypted),
                                                 bytes::Bytes{0x20}, "kernel download", kKernelStartPolicy);
        !initialized.has_value())
    {
        return initialized;
    }

    LogInfo(context, "Uploading kernel, please wait...");
    if (const Status uploaded = UploadBeefKernelBlocks(
            context, *encrypted, kernel.load_address, kKernelUploadBlockSize,
            [&context](bytes::ByteView request) { return DiscardB6Reply(context, request, kKernelTransferTimeout); });
        !uploaded.has_value())
    {
        return uploaded;
    }
    LogDebug(context, std::format("Data bytes sent: 0x{}", encrypted->size()));

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
    return context.session.StartedKernelId().transform([](const std::string&) {});
}

// The family's BEEF exchange as a callable for the shared read and write paths.
auto Exchanger(Context& context)
{
    return [&context](bytes::Byte opcode, bytes::ByteView payload, std::size_t minimum_payload,
                      std::chrono::milliseconds timeout)
    { return context.session.Exchange(opcode, payload, minimum_payload, timeout); };
}

} // namespace

Result<Iso15765Config> SubaruDensoSh7058CanDieselExecutor::Setup(const SubaruDensoSh7058CanDieselPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Status SubaruDensoSh7058CanDieselExecutor::BeforeTransportConfigure(ICanFlashTransport& transport, IClock& clock,
                                                                    const ICancellationToken& cancellation) const
{
    // The legacy execute() sequence (revision 59f4e442:64-78) resets the
    // serial connection, waits exactly 500 ms, then applies the ISO-15765
    // setters and opens the port.
    // BoundAttempt owns the lifecycle; this hook preserves the family-specific
    // pre-config ordering without exposing desktop serial details here.
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before CAN reset");
    }
    if (const Status reset = transport.ResetConnection(); !reset.has_value())
    {
        return reset;
    }
    return clock.Sleep(500ms, cancellation);
}

Result<FlashExecutionResult> SubaruDensoSh7058CanDieselExecutor::Run(const SubaruDensoSh7058CanDieselPlan& family,
                                                                     const FlashPlan& plan,
                                                                     ICanFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{
        cancellation,
        events,
        clock,
        transport,
        uds_client,
        channel,
        BeefCanSession(transport, clock, cancellation, events, channel, kSubaruDensoSh7058CanDieselBeefProfile)};

    return RunBeefKernelOperation(
        plan, "Subaru 07+ Diesel 32-bit", context, ConnectBootloader, UploadKernel,
        [](Context& c, const FlashPlan& p, PhaseReporter& progress)
        { return ReadBeefMemory(c, p.TransferRegion(), progress, Exchanger(c)); },
        [](Context& c, const FlashPlan& p, PhaseSequence& phases, PhaseReporter& compare)
        { return c.session.WriteMemory(p, phases, compare); });
}

} // namespace fastecu::flash
