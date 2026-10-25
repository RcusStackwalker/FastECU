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
constexpr std::chrono::milliseconds kLongTimeout{800};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kKernelProbeDelay{200};
constexpr std::chrono::milliseconds kKernelStartDelay{100};
constexpr std::chrono::milliseconds kKernelStartReadTimeout{10};
constexpr std::chrono::milliseconds kKernelTransferTimeout = kMediumTimeout;

constexpr std::size_t kKernelUploadBlockSize = 128;

constexpr bytes::Byte kKernelId = 0x01;

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
    std::uint32_t request_id;
    std::uint32_t response_id;
    uds::UdsClient& uds;
    CanFlashUdsChannel& channel;
    BeefCanSession session;
};

Result<std::optional<bytes::Bytes>> RawChannelRequestOptional(Context& context, bytes::ByteView pdu,
                                                              std::chrono::milliseconds timeout,
                                                              std::chrono::milliseconds delay = 0ms)
{
    if (const Status sent = SendChannelRequest(context, pdu, delay); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    // BEEF's legacy operator diagnostics inspect the complete serial frame,
    // including the CAN envelope, before deciding between the two exact
    // records.  Do not route this one exchange through the validating UDS
    // channel, which intentionally strips that evidence.
    return context.transport.Read(timeout, context.cancellation);
}

void LogOracleFrameFailure(Context& context, bytes::ByteView frame, std::size_t nrc_offset)
{
    if (frame.size() <= nrc_offset)
    {
        LogError(context, "No valid response from ECU");
        return;
    }
    LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(frame.subspan(nrc_offset))));
}

std::size_t StrictOracleNrcOffset(bytes::ByteView request)
{
    // Security seed/key records inspect received.mid(4), while every other
    // strict exchange in connect_bootloader()/upload_kernel() inspects
    // received.mid(8). Keep this call-site distinction instead of allowing
    // the UDS NRC decoder to normalize it away.
    return !request.empty() && request.front() == uds::kSidSecurityAccess ? 4U : 8U;
}

void LogStrictFrameFailure(Context& context, bytes::ByteView request, bytes::ByteView frame)
{
    // Each strict legacy call first checks received.length() > 5. The
    // diagnostic slice is then independent: a non-security seven-byte
    // negative frame reaches mid(8), which is empty and deliberately renders
    // "Not a valid answer" rather than decoding its real NRC at byte four.
    if (frame.size() <= 5)
    {
        LogError(context, "No valid response from ECU");
        return;
    }
    const std::size_t nrc_offset = StrictOracleNrcOffset(request);
    const bytes::ByteView nrc = frame.size() > nrc_offset ? frame.subspan(nrc_offset) : bytes::ByteView{};
    LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(nrc)));
}

void LogStrictFailure(Context& context, bytes::ByteView request, const Error& failure)
{
    if (failure.kind == ErrorKind::kTimeout)
    {
        LogError(context, "No valid response from ECU");
        return;
    }
    if (failure.kind != ErrorKind::kBadResponse)
    {
        return;
    }

    if (const auto& raw = context.channel.LastReceivedFrame(); raw.has_value())
    {
        LogStrictFrameFailure(context, request, *raw);
        return;
    }

    std::string detail = failure.detail;
    if (detail.starts_with("malformed UDS response:") || detail.starts_with("expected CAN reply id") ||
        detail.starts_with("expected response to SID") || detail.starts_with("unexpected response during") ||
        detail.starts_with("invalid diesel") || detail.starts_with("short "))
    {
        detail = "Not a valid answer";
    }
    LogError(context, std::format("Wrong response from ECU: {}", detail));
}

Result<bytes::Bytes> StrictRequest(Context& context, bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, policy, context.cancellation);
    if (!reply.has_value())
    {
        LogStrictFailure(context, pdu, reply.error());
        return std::unexpected(reply.error());
    }
    if (const auto& raw = context.channel.LastReceivedFrame();
        raw.has_value() && !pdu.empty() && pdu.front() == uds::kSidSecurityAccess && raw->size() <= 5)
    {
        LogStrictFrameFailure(context, pdu, *raw);
        return Fail(ErrorKind::kBadResponse, "strict CAN response is shorter than the legacy minimum");
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
        LogError(context, "Wrong response from ECU: Not a valid answer");
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
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
        LogError(context, "Wrong response from ECU: Not a valid answer");
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
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

// A kernel ID reply: the whole CAN frame, kept for failure logs, and its PDU.
struct KernelIdReply
{
    bytes::Bytes frame;
    bytes::Bytes pdu;
};

// Empty when the frame is not addressed from the ECU.
std::optional<KernelIdReply> SplitEnvelope(const Context& context, bytes::Bytes frame)
{
    if (frame.size() < CanFlashUdsChannel::kEnvelopeSize || bytes::ReadU32Be(frame) != context.response_id)
    {
        return std::nullopt;
    }
    bytes::Bytes pdu(frame.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), frame.end());
    return KernelIdReply{std::move(frame), std::move(pdu)};
}

// The initial legacy probe reads the raw serial buffer and continues when the
// frame is short, malformed, or addressed to another CAN id: those return no
// reply. The strict post-upload probe keeps the validating channel receive.
Result<std::optional<KernelIdReply>> ProbeKernelIdTolerantly(Context& context, bytes::ByteView request,
                                                             bool *response_received)
{
    if (const Status checkpoint = CancelledIfRequested(context, "cancelled before CAN request");
        !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = context.channel.Send(request, context.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (const Status slept = context.clock.Sleep(kKernelProbeDelay, context.cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    Result<std::optional<bytes::Bytes>> raw = context.transport.Read(kLongTimeout, context.cancellation);
    if (!raw.has_value())
    {
        if (raw.error().kind == ErrorKind::kCancelled || raw.error().kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(raw.error());
        }
        return std::nullopt;
    }
    if (!raw->has_value())
    {
        return std::nullopt;
    }
    if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (response_received != nullptr)
    {
        *response_received = true;
    }
    const bytes::Bytes frame = **raw;
    std::optional<KernelIdReply> reply = SplitEnvelope(context, frame);
    if (!reply.has_value())
    {
        LogOracleFrameFailure(context, frame, 8);
    }
    return reply;
}

// A timeout means no kernel is running; a frame from another CAN id fails.
Result<std::optional<KernelIdReply>> ProbeKernelIdStrictly(Context& context, bytes::ByteView request,
                                                           bool *response_received)
{
    Result<std::optional<bytes::Bytes>> raw =
        RawChannelRequestOptional(context, request, kLongTimeout, kKernelProbeDelay);
    if (!raw.has_value())
    {
        if (raw.error().kind == ErrorKind::kTimeout)
        {
            return std::nullopt;
        }
        return std::unexpected(raw.error());
    }
    if (!raw->has_value())
    {
        return std::nullopt;
    }
    if (response_received != nullptr)
    {
        *response_received = true;
    }
    const bytes::Bytes frame = **raw;
    std::optional<KernelIdReply> reply = SplitEnvelope(context, frame);
    if (!reply.has_value())
    {
        LogOracleFrameFailure(context, frame, 8);
        return std::unexpected(Error{ErrorKind::kBadResponse, "unexpected CAN response id"});
    }
    return reply;
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed,
                                                   bool *response_received = nullptr)
{
    // request_kernel_id(), revision 59f4e442:1797-1829. The three trailing
    // zero bytes are outside the declared BEEF body but are present on the
    // legacy wire and therefore remain literal here.
    const bytes::Bytes request{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
    Result<std::optional<KernelIdReply>> reply = tolerate_malformed
                                                     ? ProbeKernelIdTolerantly(context, request, response_received)
                                                     : ProbeKernelIdStrictly(context, request, response_received);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return std::optional<std::string>{};
    }
    Result<BeefMessage> parsed = ParseBeef((*reply)->pdu);
    if (parsed.has_value() && parsed->opcode == static_cast<bytes::Byte>(kKernelId | 0x40U))
    {
        return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
    }
    LogOracleFrameFailure(context, (*reply)->frame, 8);
    if (tolerate_malformed)
    {
        return std::optional<std::string>{};
    }
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
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

// Requests programming in each session mode the ECU accepted earlier.
Status EnterProgrammingSession(Context& context, bool connected_03, bool connected_43)
{
    LogInfo(context, "Set session mode");
    bytes::Bytes programming{uds::kSidDiagnosticSessionControl};
    if (connected_03)
    {
        programming.push_back(0x02);
    }
    if (connected_43)
    {
        programming.push_back(0x42);
    }
    Result<bytes::Bytes> programming_reply = StrictRequest(context, programming, kStrictPolicy);
    if (!programming_reply.has_value())
    {
        return std::unexpected(programming_reply.error());
    }
    const bytes::ByteView programming_payload = uds::Payload(*programming_reply);
    if (programming_payload.empty() || (programming_payload[0] != 0x02 && programming_payload[0] != 0x42))
    {
        LogError(context, "Wrong response from ECU: Not a valid answer");
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(context, "Succesfully set to programming session");
    return {};
}

Status ConnectBootloader(Context& context, bool read_operation, bool& kernel_alive, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442:113-510.
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    bool kernel_probe_received = false;
    Result<std::optional<std::string>> kernel_id = RequestKernelId(context, true, &kernel_probe_received);
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        LogInfo(context, std::format("Kernel ID: {}", **kernel_id));
        kernel_alive = true;
        return {};
    }
    if (!kernel_probe_received)
    {
        LogError(context, "No valid response from ECU");
    }
    LogInfo(context, "No response from kernel, initialising ECU...");
    LogInfo(context, "Initializing connection...");
    if (Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
    {
        return identity;
    }

    LogInfo(context, "Requesting session mode");
    Result<bool> connected_03 = context.session.RequestSession(0x03);
    if (!connected_03.has_value())
    {
        return std::unexpected(connected_03.error());
    }
    Result<bool> connected_43 = context.session.RequestSession(0x43);
    if (!connected_43.has_value())
    {
        return std::unexpected(connected_43.error());
    }
    if (Status unlocked = UnlockSecurity(context); !unlocked.has_value())
    {
        return unlocked;
    }
    return EnterProgrammingSession(context, *connected_03, *connected_43);
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
    Result<std::optional<std::string>> kernel_id = RequestKernelId(context, false);
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (!kernel_id->has_value())
    {
        LogError(context, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "kernel did not answer after upload");
    }
    LogInfo(context, std::format("Kernel ID: {}", **kernel_id));
    return {};
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
        family.request_id,
        family.response_id,
        uds_client,
        channel,
        BeefCanSession(transport, clock, cancellation, events, channel, kSubaruDensoSh7058CanDieselBeefProfile)};

    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    bool kernel_alive = false;
    std::optional<std::string> rom_id;
    LogInfo(context, "Connecting to Subaru 07+ Diesel 32-bit CAN bootloader, please wait...");
    if (const Status connected = ConnectBootloader(context, read_operation, kernel_alive, rom_id);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    if (!kernel_alive)
    {
        events.Notice("Preparing, please wait...");
        LogInfo(context, "Initializing Subaru 07+ Diesel 32-bit CAN kernel upload, please wait...");
        if (const Status uploaded = UploadKernel(context, plan.KernelOrEmpty()); !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }
    kernel_phase.Complete();

    if (read_operation)
    {
        events.Notice("Reading ROM, please wait...");
        LogInfo(context, "Reading ROM from Subaru 07+ Diesel 32-bit using CAN");
        PhaseReporter read_phase = phases.Start("Read", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = ReadBeefMemory(context, plan.TransferRegion(), read_phase, Exchanger(context));
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead, .read_bytes = std::move(*rom), .rom_id = std::move(rom_id)};
    }

    events.Notice("Writing ROM, please wait...");
    LogInfo(context, "Writing ROM to Subaru 07+ Diesel 32-bit using CAN");
    PhaseReporter compare_phase = phases.Start("Compare", static_cast<int>(plan.EraseRegions().size()));
    if (const Status written = context.session.WriteMemory(plan, phases, compare_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    PhaseReporter complete = phases.Start("Complete", 1);
    complete.Complete();
    return FlashExecutionResult{.operation = plan.Operation()};
}

} // namespace fastecu::flash
