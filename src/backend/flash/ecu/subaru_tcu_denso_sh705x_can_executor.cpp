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
#include "src/backend/flash/ecu/beef_can_session.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
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
    BeefCanSession session;
    BeefCanKernel kernel;
};

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    return DensoSeedKey(seed);
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

Status SendKernelIdProbe(Context& context, bytes::ByteView request, bytes::ByteView wire_request)
{
    if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = context.channel.Send(request, context.cancellation); !sent.has_value())
    {
        return sent;
    }
    LogInfo(context, std::format("Kernel ID request: {}", bytes::ToHex(wire_request)));
    return context.clock.Sleep(kProbeDelay, context.cancellation);
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
    Result<BeefMessage> parsed = ParseBeef(coalesced);
    if (!parsed.has_value())
    {
        return Malformed(tolerate_malformed, parsed.error());
    }
    if (parsed->opcode != kKernelId)
    {
        LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(coalesced)));
        return Malformed(tolerate_malformed, Error{ErrorKind::kBadResponse, "unexpected kernel ID response opcode"});
    }
    return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed)
{
    // request_kernel_id(), revision 59f4e442 lines 1583-1647. The literal
    // PDU is intentionally kept here instead of deriving it from BEEF
    // constants: 0x7A 0xA0 is the legacy kernel probe, not a BEEF command.
    const bytes::Bytes request{0x7A, 0xA0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    const bytes::Bytes wire_request = ComposeBe(context.request_id, request);
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (const Status sent = SendKernelIdProbe(context, request, wire_request); !sent.has_value())
        {
            return std::unexpected(sent.error());
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

Status StrictPayload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label,
                     std::optional<std::size_t> minimum_size = std::nullopt)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, kStrictPolicy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    const std::size_t required = minimum_size.value_or(expected.size());
    if (payload.size() < required || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        LogError(context, std::format("Wrong response from TCU during {}", label));
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
    }
    return {};
}

Status StrictSeed(Context& context, bytes::Bytes& key)
{
    Result<bytes::Bytes> seed_reply = context.uds.Request(
        bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, kStrictPolicy, context.cancellation);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*seed_reply);
    if (payload.size() < 5 || payload[0] != uds::kSecurityAccessRequestSeed)
    {
        return Fail(ErrorKind::kBadResponse, "invalid TCU seed response");
    }
    key = SeedKey(payload.subspan(1, 4));
    return {};
}

// The ECU/CAL identity queries (lines 137-214) are intentionally non-fatal.
// Their successful values form the legacy read ROM id.
Status RequestIdentity(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    if (Status ecu_id = context.session.RequestEcuId(read_operation, rom_id); !ecu_id.has_value())
    {
        return ecu_id;
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
    bytes::Bytes key;
    if (const Status seed = StrictSeed(context, key); !seed.has_value())
    {
        return seed;
    }
    LogInfo(context, "Seed request ok");
    LogInfo(context, "Sending seed key");
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    if (const Status key_status = StrictPayload(context, key_request, bytes::Bytes{0x02}, "seed key");
        !key_status.has_value())
    {
        return key_status;
    }
    LogInfo(context, "Seed key ok");
    return EnterBeefProgrammingSession(context, true, false, kStrictPolicy);
}

Status ConnectBootloader(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442 lines 107-135. The oracle
    // continues initialization after any nonterminal invalid probe reply;
    // post-upload callers select the strict path instead.
    if (Status identity = RequestIdentity(context, read_operation, rom_id); !identity.has_value())
    {
        return identity;
    }
    return EnterProgrammingSession(context);
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
        BeefCanSession(transport, clock, cancellation, events, channel, kSubaruTcuDensoSh705xCanBeefProfile),
        BeefCanKernel(transport, clock, cancellation, events, channel, kSubaruTcuDensoSh705xCanBeefProfile)};

    return RunBeefKernelOperation(
        plan, "Subaru Denso SH705x TCU", context, [](Context& c) { return RequestKernelId(c, true); },
        ConnectBootloader, UploadKernel, [](Context& c, const FlashPlan& p, PhaseReporter& progress)
        { return c.kernel.Read(p.TransferRegion(), progress); },
        [](Context& c, const FlashPlan& p, PhaseSequence& phases, PhaseReporter& compare)
        { return c.kernel.Write(p, phases, compare); });
}

} // namespace fastecu::flash
