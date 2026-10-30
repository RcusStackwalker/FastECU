#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_executor.h"

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
#include "src/backend/flash/ecu/beef_can_kernel.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_tcu_denso_sh705x_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every protocol exchange below is transcribed from
// src/platform/desktop/common/flash/legacy/tcu/
// flash_tcu_subaru_denso_sh705x_can_operation.cpp at revision 59f4e442:
// connect_bootloader (93-364), upload_kernel (369-627), read_mem (632-779),
// write_mem (784-900), CRC/init/reflash/flash (905-1517), seed/crypto
// (1522-1577), and request_kernel_id (1583-1647).
//
// ISO-15765 is only the envelope transport here. Strict UDS exchanges use
// uds::UdsClient; the tolerant 0xAA/0x09 identity queries and all BEEF
// kernel commands are sent through the channel directly because they are
// vendor/proprietary messages rather than UDS application PDUs.

namespace fastecu::flash
{
namespace
{

using bytes::ComposeBe;
using namespace std::chrono_literals;

constexpr std::chrono::milliseconds kProbeDelay{100}; // request_kernel_id(), lines 1608-1609
constexpr std::chrono::milliseconds kExtraShortTimeout{50};
constexpr std::chrono::milliseconds kShortTimeout{200};
constexpr std::chrono::milliseconds kLongTimeout{800};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kPostUploadDelay{500}; // upload_kernel(), lines 595-595

constexpr bytes::Byte kKernelId = 0x41;

constexpr uds::ExchangePolicy kStrictPolicy{
    .pre_read_delay = kExtraShortTimeout,
    .read_timeout = kReadTimeout,
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
    uds::IUdsChannel& channel;
    BeefCanKernel kernel;
};

Status CancelledIfRequested(const Context& context, std::string_view detail)
{
    if (context.cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::string(detail));
    }
    return {};
}

void LogWrongPdu(const Context& context, bytes::ByteView pdu)
{
    LogError(context, std::format("Wrong response from TCU: {}", bytes::ToHex(pdu)));
}

Result<std::optional<bytes::Bytes>> NonfatalQuery(Context& context, bytes::ByteView pdu)
{
    auto received = CancelledIfRequested(context, "cancelled before CAN request")
                        .and_then([&] { return context.channel.Send(pdu, context.cancellation); })
                        .and_then([&] { return context.clock.Sleep(kExtraShortTimeout, context.cancellation); })
                        .and_then([&] { return context.channel.Receive(kReadTimeout, context.cancellation); });
    if (!received.has_value())
    {
        const Error& failure = received.error();
        if (failure.kind == ErrorKind::kCancelled || failure.kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(failure);
        }
        LogError(context, failure.kind == ErrorKind::kTimeout
                              ? "No valid response from ECU"
                              : std::format("Wrong response from TCU: {}", failure.detail));
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        LogError(context, "No valid response from ECU");
    }
    return received;
}

bool IsFatal(const Error& error)
{
    return error.kind == ErrorKind::kCancelled || error.kind == ErrorKind::kDisconnected;
}

// A malformed kernel ID reply means "no kernel" to a tolerant probe and fails
// a strict one.
Result<std::optional<std::string>> Malformed(bool tolerate_malformed, Error error)
{
    if (tolerate_malformed)
    {
        return std::optional<std::string>{};
    }
    return std::unexpected(std::move(error));
}

// Legacy lines 1621-1625 keep reading at the short timeout and append every
// returned continuation. ICanFlashTransport replaces the legacy adapter's
// two-byte prefix with a four-byte arbitration-id envelope, so each accepted
// continuation contributes every byte after that envelope. The drain is
// bounded and stays cancellation and disconnect aware; false means the bound
// ran out before the queue emptied.
Result<bool> DrainKernelIdFragments(Context& context, bytes::Bytes& coalesced)
{
    constexpr int kMaxKernelIdTrailingFrames = 32;
    for (int trailing = 0; trailing < kMaxKernelIdTrailingFrames; ++trailing)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "kernel ID trailing drain cancelled");
            !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        Result<std::optional<bytes::Bytes>> fragment = context.transport.Read(kShortTimeout, context.cancellation);
        if (!fragment.has_value())
        {
            if (fragment.error().kind == ErrorKind::kTimeout)
            {
                return true;
            }
            return std::unexpected(fragment.error());
        }
        if (!fragment->has_value())
        {
            return true;
        }
        const bytes::Bytes& fragment_raw = **fragment;
        if (fragment_raw.size() < CanFlashUdsChannel::kEnvelopeSize ||
            bytes::ReadU32Be(fragment_raw) != context.response_id)
        {
            continue;
        }
        coalesced.insert(coalesced.end(),
                         fragment_raw.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize),
                         fragment_raw.end());
    }
    return false;
}

Result<std::optional<std::string>> ParseKernelIdReply(Context& context, const bytes::Bytes& raw,
                                                      bool tolerate_malformed)
{
    if (raw.size() < CanFlashUdsChannel::kEnvelopeSize)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "short CAN frame in kernel ID response"});
    }
    if (bytes::ReadU32Be(raw) != context.response_id)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "wrong CAN id in kernel ID response"});
    }
    bytes::Bytes coalesced(raw.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), raw.end());
    Result<bool> drained = DrainKernelIdFragments(context, coalesced);
    if (!drained.has_value())
    {
        return IsFatal(drained.error()) ? std::unexpected(drained.error())
                                        : Malformed(tolerate_malformed, drained.error());
    }
    if (!*drained)
    {
        // Never begin another command while the raw receive queue may still
        // contain ID fragments. This is the bounded portable form of the
        // legacy read-until-empty loop.
        return Malformed(tolerate_malformed,
                         Error{ErrorKind::kBadResponse, "kernel ID trailing drain exceeded its frame bound"});
    }
    if (coalesced.size() < 5)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "short BEEF response envelope"});
    }
    if (bytes::ReadU16Be(coalesced) != 0xBEEF)
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "wrong BEEF response marker"});
    }
    const auto declared = bytes::ReadU16Be(coalesced, 2);
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > coalesced.size())
    {
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "invalid BEEF response length"});
    }
    if (coalesced[4] != kKernelId)
    {
        LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(coalesced)));
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "unexpected kernel ID response opcode"});
    }
    return std::optional<std::string>{std::string(coalesced.begin() + 5, coalesced.begin() + declared + 4)};
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed)
{
    // request_kernel_id(), revision 59f4e442 lines 1583-1647. The literal
    // PDU is intentionally kept here instead of deriving it from BEEF
    // constants: 0x7A 0xA0 is the legacy kernel probe, not a BEEF command.
    const bytes::Bytes request{0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled");
            !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        if (const Status sent = context.channel.Send(request, context.cancellation); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }
        LogInfo(context, std::format("Kernel ID request: {}", bytes::ToHex(ComposeBe(context.request_id, request))));
        if (const Status delayed = context.clock.Sleep(kProbeDelay, context.cancellation); !delayed.has_value())
        {
            return std::unexpected(delayed.error());
        }
        Result<std::optional<bytes::Bytes>> received = context.transport.Read(kLongTimeout, context.cancellation);
        LogInfo(context,
                std::format("Kernel ID response: {}",
                            received.has_value() && received->has_value() ? bytes::ToHex(**received) : std::string{}));
        if (!received.has_value())
        {
            if (received.error().kind == ErrorKind::kTimeout)
            {
                continue;
            }
            return IsFatal(received.error()) ? std::unexpected(received.error())
                                             : Malformed(tolerate_malformed, received.error());
        }
        if (received->has_value())
        {
            return ParseKernelIdReply(context, **received, tolerate_malformed);
        }
    }
    return std::optional<std::string>{};
}

Status StrictPayload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, kStrictPolicy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.size() < expected.size() || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        LogError(context, std::format("Wrong response from TCU during {}", label));
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
    }
    return {};
}

// The ECU/CAL identity queries (lines 137-214) are intentionally non-fatal.
// Their successful values form the legacy read ROM id.
Status RequestIdentity(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    LogInfo(context, "Requesting ECU ID");
    const auto ecu_reply = NonfatalQuery(context, bytes::Bytes{0xAA});
    if (!ecu_reply.has_value())
    {
        return std::unexpected(ecu_reply.error());
    }
    if (ecu_reply->has_value())
    {
        const auto& pdu = **ecu_reply;
        if (pdu.size() < 9 || pdu[0] != 0xEA)
        {
            LogWrongPdu(context, pdu);
        }
        else
        {
            const std::string ecu_id =
                std::format("{:02X}{:02X}{:02X}{:02X}{:02X}", pdu[4], pdu[5], pdu[6], pdu[7], pdu[8]);
            LogInfo(context, std::format("ECU ID: {}", ecu_id));
            if (read_operation)
            {
                rom_id = ecu_id + "_";
            }
        }
    }
    LogInfo(context, "Requesting CAL ID");
    const auto cal_id = NonfatalQuery(context, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId});
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (cal_id->has_value())
    {
        const auto& pdu = **cal_id;
        if (pdu.size() < 3 || pdu[0] != 0x49 || pdu[1] != 0x04)
        {
            LogWrongPdu(context, pdu);
        }
        else
        {
            const std::string value(pdu.begin() + 3, pdu.end());
            LogInfo(context, std::format("CAL ID: {}", value));
            if (read_operation)
            {
                rom_id = value + "_" + rom_id.value_or(std::string{});
            }
        }
    }
    return {};
}

// Strict UDS session/security exchanges, lines 216-359. Oracle line 230
// joins its two mismatches with && and therefore accepts 50 02 or 51 03;
// requiring both fields is the documented automated-only safety fix.
Status EnterProgrammingSession(Context& context)
{
    LogInfo(context, "Requesting session mode");
    if (const Status session = StrictPayload(context, bytes::Bytes{uds::kSidDiagnosticSessionControl, 0x03},
                                             bytes::Bytes{0x03}, "session mode");
        !session.has_value())
    {
        return session;
    }
    const auto seed_reply = context.uds.Request(bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
                                                kStrictPolicy, context.cancellation);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView seed = uds::Payload(*seed_reply);
    if (seed.size() < 5 || seed[0] != uds::kSecurityAccessRequestSeed)
    {
        return Fail(ErrorKind::kBadResponse, "invalid TCU seed response");
    }
    const bytes::Bytes key = DensoSeedKey(seed.subspan<1, 4>());
    LogInfo(context, "Seed request ok");
    LogInfo(context, "Sending seed key");
    if (const Status key_status =
            StrictPayload(context, ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, key),
                          bytes::Bytes{0x02}, "seed key");
        !key_status.has_value())
    {
        return key_status;
    }
    LogInfo(context, "Seed key ok");
    LogInfo(context, "Set session mode");
    const auto reply =
        context.uds.Request(bytes::Bytes{uds::kSidDiagnosticSessionControl, 0x02}, kStrictPolicy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const auto payload = uds::Payload(*reply);
    if (payload.empty() || (payload[0] != 0x02 && payload[0] != 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(context, "Succesfully set to programming session");
    return {};
}

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    if (const Status uploaded = context.kernel.TransferPayload(kernel, {kStrictPolicy, kReadTimeout});
        !uploaded.has_value())
    {
        return uploaded;
    }
    LogInfo(context, "Kernel uploaded, starting...");
    if (const Status transfer_exit =
            StrictPayload(context, bytes::Bytes{uds::kSidRequestTransferExit}, {}, "kernel transfer exit");
        !transfer_exit.has_value())
    {
        return transfer_exit;
    }
    // upload_kernel(), revision 59f4e442 lines 546-577 checks only the
    // positive service id, so an echoed routine is optional.
    if (const Status start_kernel =
            StrictPayload(context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02}, {}, "kernel start");
        !start_kernel.has_value())
    {
        return start_kernel;
    }

    LogInfo(context, "Kernel started, initializing...");
    // upload_kernel(), revision 59f4e442 lines 581-595 resets the adapter
    // and reapplies the TCU's ISO-15765 configuration before its quiet
    // period and strict post-upload probe. This is protocol-owned inner
    // lifecycle; BoundAttempt retains the initial configure/open and final
    // close ownership.
    if (const Status restarted = context.transport.RestartIso15765({.bitrate = 500000,
                                                                    .request_id = context.request_id,
                                                                    .response_id = context.response_id,
                                                                    .extended_id = false},
                                                                   context.cancellation);
        !restarted.has_value())
    {
        return restarted;
    }
    if (const Status slept = context.clock.Sleep(kPostUploadDelay, context.cancellation); !slept.has_value())
    {
        return slept;
    }
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = RequestKernelId(context, false);
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
        return Fail(ErrorKind::kTimeout, "kernel did not answer after upload");
    }
    return {};
}

} // namespace

Result<Iso15765Config> SubaruTcuDensoSh705xCanExecutor::Setup(const SubaruTcuDensoSh705xCanPlan& family) const
{
    return Iso15765ConfigFrom(family);
}

Result<FlashExecutionResult> SubaruTcuDensoSh705xCanExecutor::Run(const SubaruTcuDensoSh705xCanPlan& family,
                                                                  const FlashPlan& plan, ICanFlashTransport& transport,
                                                                  IClock& clock, const ICancellationToken& cancellation,
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
        BeefCanKernel(transport, clock, cancellation, events, channel, kSubaruTcuDensoSh705xCanBeefProfile)};

    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(context.events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    std::optional<std::string> rom_id;
    LogInfo(context, "Connecting to Subaru Denso SH705x TCU CAN bootloader, please wait...");
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    const auto kernel_id = RequestKernelId(context, true);
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
        if (const Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
        {
            return std::unexpected(identity.error());
        }
        if (const Status entered = EnterProgrammingSession(context); !entered.has_value())
        {
            return std::unexpected(entered.error());
        }
        if (plan.Kernel().has_value())
        {
            context.events.Notice("Preparing, please wait...");
            LogInfo(context, "Initializing Subaru Denso SH705x TCU CAN kernel upload, please wait...");
            if (const Status uploaded = UploadKernel(context, *plan.Kernel()); !uploaded.has_value())
            {
                return std::unexpected(uploaded.error());
            }
        }
    }
    kernel_phase.Complete();
    if (read_operation)
    {
        context.events.Notice("Reading ROM, please wait...");
        LogInfo(context, "Reading ROM from Subaru Denso SH705x TCU using CAN");
        PhaseReporter read_phase = phases.Start("Read", static_cast<int>(plan.TransferRegion().length));
        auto rom = context.kernel.Read(plan.TransferRegion(), read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead, .read_bytes = std::move(*rom), .rom_id = std::move(rom_id)};
    }
    context.events.Notice("Writing ROM, please wait...");
    LogInfo(context, "Writing ROM to Subaru Denso SH705x TCU using CAN");
    PhaseReporter compare_phase = phases.Start("Compare", static_cast<int>(plan.EraseRegions().size()));
    if (const Status written = context.kernel.Write(plan, phases, compare_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    PhaseReporter complete = phases.Start("Complete", 1);
    complete.Complete();
    return FlashExecutionResult{.operation = plan.Operation()};
}

} // namespace fastecu::flash
