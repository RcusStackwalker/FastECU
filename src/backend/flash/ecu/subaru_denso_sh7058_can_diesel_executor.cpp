#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_executor.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
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
constexpr std::chrono::milliseconds kShortTimeout{200};
constexpr std::chrono::milliseconds kMediumTimeout{500};
constexpr std::chrono::milliseconds kLongTimeout{800};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kKernelProbeDelay{200};
constexpr std::chrono::milliseconds kKernelStartDelay{100};
constexpr std::chrono::milliseconds kKernelStartReadTimeout{10};
constexpr std::chrono::milliseconds kKernelTransferTimeout = kMediumTimeout;

constexpr std::size_t kKernelPageSize = 0x400;
constexpr std::size_t kKernelUploadBlockSize = 128;
constexpr std::size_t kFlashBufferSize = 0x200;
constexpr std::size_t kFlashCommitSize = 0x1000;

constexpr bytes::Byte kKernelId = 0x01;
constexpr bytes::Byte kKernelCrc = 0x02;
constexpr bytes::Byte kKernelReadArea = 0x03;
constexpr bytes::Byte kKernelProgVolt = 0x04;
constexpr bytes::Byte kKernelGetMaxMessage = 0x05;
constexpr bytes::Byte kKernelGetMaxBlock = 0x06;
constexpr bytes::Byte kKernelFlashEnable = 0x20;
constexpr bytes::Byte kKernelFlashDisable = 0x21;
constexpr bytes::Byte kKernelWriteBuffer = 0x22;
constexpr bytes::Byte kKernelValidateBuffer = 0x23;
constexpr bytes::Byte kKernelCommitBuffer = 0x24;
constexpr bytes::Byte kKernelBlankPage = 0x25;

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
};

std::string UppercaseHexCompact(bytes::ByteView data)
{
    std::string rendered;
    rendered.reserve(data.size() * 2);
    for (const bytes::Byte value : data)
    {
        std::format_to(std::back_inserter(rendered), "{:02X}", value);
    }
    return rendered;
}

struct BeefMessage
{
    bytes::Byte opcode;
    bytes::ByteView payload;
};

Result<BeefMessage> ParseBeef(bytes::ByteView pdu)
{
    if (pdu.size() < 5)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response envelope");
    }
    if (bytes::ReadU16Be(pdu) != kKernelStartComm)
    {
        return Fail(ErrorKind::kBadResponse, "wrong BEEF response marker");
    }
    const std::uint16_t declared = bytes::ReadU16Be(pdu, 2);
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > pdu.size())
    {
        return Fail(ErrorKind::kBadResponse, "invalid BEEF response length");
    }
    return BeefMessage{.opcode = pdu[4], .payload = pdu.subspan(5, declared - 1)};
}

Result<std::optional<bytes::Bytes>> RawChannelRequestOptional(Context& context, bytes::ByteView pdu,
                                                              std::chrono::milliseconds timeout,
                                                              std::chrono::milliseconds delay = 0ms)
{
    if (const Status checkpoint = CancelledIfRequested(context, "cancelled before CAN request");
        !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = context.channel.Send(pdu, context.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay > 0ms)
    {
        if (const Status slept = context.clock.Sleep(delay, context.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
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

Status UploadB6Discard(Context& context, bytes::ByteView pdu)
{
    // upload_kernel(), revision 59f4e442:517-782 unconditionally reads and
    // discards every B6 reply. Send through the channel for the request CAN
    // envelope, but read the reply directly so short/malformed/wrong-ID
    // frames and adapter-specific stale errors remain non-fatal like legacy.
    if (const Status checkpoint = CancelledIfRequested(context, "kernel upload cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = context.channel.Send(pdu, context.cancellation); !sent.has_value())
    {
        return sent;
    }
    const Result<std::optional<bytes::Bytes>> ignored =
        context.transport.Read(kKernelTransferTimeout, context.cancellation);
    if (!ignored.has_value() &&
        (ignored.error().kind == ErrorKind::kCancelled || ignored.error().kind == ErrorKind::kDisconnected))
    {
        return std::unexpected(ignored.error());
    }
    if (const Status checkpoint = CancelledIfRequested(context, "kernel upload cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    return {};
}

Result<bytes::Bytes> BeefExchange(Context& context, bytes::Byte opcode, bytes::ByteView payload,
                                  std::size_t minimum_payload, std::chrono::milliseconds timeout)
{
    Result<std::optional<bytes::Bytes>> received =
        RawChannelRequestOptional(context, BeefRequest(opcode, payload), timeout);
    if (!received.has_value())
    {
        if (received.error().kind != ErrorKind::kCancelled && received.error().kind != ErrorKind::kDisconnected)
        {
            LogError(context, "No valid response from ECU");
        }
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        LogError(context, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "no CAN response within the read timeout");
    }

    const bytes::Bytes& frame = **received;
    if (frame.size() < CanFlashUdsChannel::kEnvelopeSize)
    {
        LogOracleFrameFailure(context, frame, 8);
        return Fail(ErrorKind::kBadResponse, "CAN frame is shorter than its envelope");
    }
    if (bytes::ReadU32Be(frame) != context.response_id)
    {
        LogOracleFrameFailure(context, frame, 8);
        return Fail(ErrorKind::kBadResponse, "unexpected CAN response id");
    }

    Result<BeefMessage> parsed = ParseBeef(bytes::ByteView(frame).subspan(CanFlashUdsChannel::kEnvelopeSize));
    if (!parsed.has_value())
    {
        LogOracleFrameFailure(context, frame, 8);
        return std::unexpected(parsed.error());
    }
    const bytes::Byte expected = static_cast<bytes::Byte>(opcode | 0x40U);
    if (parsed->opcode != expected)
    {
        LogOracleFrameFailure(context, frame, 8);
        return Fail(ErrorKind::kBadResponse, std::format("unexpected BEEF response opcode 0x{:02x}, expected 0x{:02x}",
                                                         parsed->opcode, expected));
    }
    if (parsed->payload.size() < minimum_payload)
    {
        LogOracleFrameFailure(context, frame, 8);
        return Fail(ErrorKind::kBadResponse, "short BEEF response payload");
    }
    return bytes::Bytes(parsed->payload.begin(), parsed->payload.end());
}

Status DiscardStaleFrame(Context& context)
{
    // check_romcrc(), revision 59f4e442:1101-1187 deliberately ignores this
    // short drain. Preserve malformed/timeout tolerance while retaining
    // cooperative cancellation and adapter-loss semantics.
    if (const Status checkpoint = CancelledIfRequested(context, "CRC stale-frame drain cancelled");
        !checkpoint.has_value())
    {
        return checkpoint;
    }
    Result<std::optional<bytes::Bytes>> stale = context.transport.Read(kShortTimeout, context.cancellation);
    if (!stale.has_value() &&
        (stale.error().kind == ErrorKind::kCancelled || stale.error().kind == ErrorKind::kDisconnected))
    {
        return std::unexpected(stale.error());
    }
    return {};
}

Result<std::optional<bytes::Bytes>> NonfatalQuery(Context& context, bytes::ByteView pdu)
{
    Result<std::optional<bytes::Bytes>> received =
        ChannelRequestOptional(context, pdu, kReadTimeout, kExtraShortTimeout);
    if (!received.has_value())
    {
        if (received.error().kind == ErrorKind::kCancelled || received.error().kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(received.error());
        }
        if (received.error().kind == ErrorKind::kTimeout)
        {
            LogError(context, "No valid response from ECU");
        }
        else
        {
            LogError(context, "Wrong response from ECU: Not a valid answer");
        }
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        LogError(context, "No valid response from ECU");
    }
    return received;
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

Result<bytes::Bytes> SecurityKey(Context& context, bytes::ByteView seed)
{
    if (seed.size() != 4)
    {
        return Fail(ErrorKind::kBadResponse, "diesel security seed must contain four bytes");
    }
    return DensoSeedKey(seed);
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed,
                                                   bool *response_received = nullptr)
{
    // request_kernel_id(), revision 59f4e442:1797-1829. The three trailing
    // zero bytes are outside the declared BEEF body but are present on the
    // legacy wire and therefore remain literal here.
    const bytes::Bytes request{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
    bytes::Bytes pdu;
    std::optional<bytes::Bytes> raw_frame;
    if (tolerate_malformed)
    {
        // The initial legacy probe reads the raw serial buffer and continues
        // when the frame is short, malformed, or addressed to another CAN
        // id. Keep that tolerance without weakening the strict post-upload
        // probe below, which still uses the validating channel receive.
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
            return std::optional<std::string>{};
        }
        if (!raw->has_value())
        {
            return std::optional<std::string>{};
        }
        if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled");
            !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const bytes::Bytes& frame = **raw;
        raw_frame = frame;
        if (response_received != nullptr)
        {
            *response_received = true;
        }
        if (frame.size() < CanFlashUdsChannel::kEnvelopeSize || bytes::ReadU32Be(frame) != context.response_id)
        {
            LogOracleFrameFailure(context, frame, 8);
            return std::optional<std::string>{};
        }
        pdu.assign(frame.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), frame.end());
    }
    else
    {
        Result<std::optional<bytes::Bytes>> reply =
            RawChannelRequestOptional(context, request, kLongTimeout, kKernelProbeDelay);
        if (!reply.has_value())
        {
            if (reply.error().kind == ErrorKind::kTimeout)
            {
                return std::optional<std::string>{};
            }
            return std::unexpected(reply.error());
        }
        if (!reply->has_value())
        {
            return std::optional<std::string>{};
        }
        raw_frame = std::move(**reply);
        if (response_received != nullptr)
        {
            *response_received = true;
        }
        const bytes::Bytes& frame = *raw_frame;
        if (frame.size() < CanFlashUdsChannel::kEnvelopeSize || bytes::ReadU32Be(frame) != context.response_id)
        {
            LogOracleFrameFailure(context, frame, 8);
            return std::unexpected(Error{ErrorKind::kBadResponse, "unexpected CAN response id"});
        }
        pdu.assign(frame.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), frame.end());
    }

    Result<BeefMessage> parsed = ParseBeef(pdu);
    if (!parsed.has_value())
    {
        if (tolerate_malformed)
        {
            LogOracleFrameFailure(context, *raw_frame, 8);
            return std::optional<std::string>{};
        }
        LogOracleFrameFailure(context, *raw_frame, 8);
        return std::unexpected(parsed.error());
    }
    if (parsed->opcode != static_cast<bytes::Byte>(kKernelId | 0x40U))
    {
        if (tolerate_malformed)
        {
            LogOracleFrameFailure(context, *raw_frame, 8);
            return std::optional<std::string>{};
        }
        LogOracleFrameFailure(context, *raw_frame, 8);
        return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
    }
    return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
}

Result<std::optional<std::string>> IdentityString(Context& context, bytes::ByteView request, bytes::Byte first,
                                                  bytes::Byte second, std::string_view label)
{
    Result<std::optional<bytes::Bytes>> reply = NonfatalQuery(context, request);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return std::optional<std::string>{};
    }
    const bytes::Bytes& pdu = **reply;
    if (pdu.size() < 3 || pdu[0] != first || pdu[1] != second)
    {
        LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(pdu)));
        return std::optional<std::string>{};
    }
    const std::string value(pdu.begin() + 3, pdu.end());
    LogInfo(context, std::format("{}: {}", label, value));
    return std::optional<std::string>{value};
}

Result<bool> RequestSession(Context& context, bytes::Byte subfunction)
{
    Result<std::optional<bytes::Bytes>> reply =
        NonfatalQuery(context, bytes::Bytes{uds::kSidDiagnosticSessionControl, subfunction});
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return false;
    }
    const bytes::Bytes& pdu = **reply;
    if (pdu.size() >= 2 && pdu[0] == 0x50 && pdu[1] == subfunction)
    {
        return true;
    }
    LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(pdu)));
    return false;
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

    std::optional<std::string> ecu_id;
    LogInfo(context, "Requesting ECU ID");
    Result<std::optional<bytes::Bytes>> ecu_reply = NonfatalQuery(context, bytes::Bytes{0xAA});
    if (!ecu_reply.has_value())
    {
        return std::unexpected(ecu_reply.error());
    }
    if (ecu_reply->has_value())
    {
        const bytes::Bytes& pdu = **ecu_reply;
        if (pdu.size() >= 9 && pdu[0] == 0xEA)
        {
            ecu_id = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}", pdu[4], pdu[5], pdu[6], pdu[7], pdu[8]);
            LogInfo(context, std::format("ECU ID: {}", *ecu_id));
            if (read_operation)
            {
                rom_id = *ecu_id + "_";
            }
        }
        else
        {
            LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(pdu)));
        }
    }

    LogInfo(context, "Requesting VIN");
    Result<std::optional<std::string>> vin =
        IdentityString(context, bytes::Bytes{uds::kSidVehicleInfoRequest, 0x02}, 0x49, 0x02, "VIN");
    if (!vin.has_value())
    {
        return std::unexpected(vin.error());
    }

    LogInfo(context, "Requesting CAL ID");
    Result<std::optional<std::string>> cal_id = IdentityString(
        context, bytes::Bytes{uds::kSidVehicleInfoRequest, uds::kVehicleInfoPidCalId}, 0x49, 0x04, "CAL ID");
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    if (read_operation && cal_id->has_value())
    {
        rom_id = **cal_id + "_" + rom_id.value_or(std::string{});
    }

    LogInfo(context, "Requesting CVN");
    Result<std::optional<bytes::Bytes>> cvn_reply =
        NonfatalQuery(context, bytes::Bytes{uds::kSidVehicleInfoRequest, 0x06});
    if (!cvn_reply.has_value())
    {
        return std::unexpected(cvn_reply.error());
    }
    if (cvn_reply->has_value())
    {
        const bytes::Bytes& pdu = **cvn_reply;
        if (pdu.size() >= 3 && pdu[0] == 0x49 && pdu[1] == 0x06)
        {
            LogInfo(context, std::format("CVN: {}", UppercaseHexCompact(bytes::ByteView(pdu).subspan(3))));
        }
        else
        {
            LogError(context, std::format("Wrong response from ECU: {}", uds::Describe(pdu)));
        }
    }

    LogInfo(context, "Requesting session mode");
    Result<bool> connected_03 = RequestSession(context, 0x03);
    if (!connected_03.has_value())
    {
        return std::unexpected(connected_03.error());
    }
    Result<bool> connected_43 = RequestSession(context, 0x43);
    if (!connected_43.has_value())
    {
        return std::unexpected(connected_43.error());
    }

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

    LogInfo(context, "Set session mode");
    bytes::Bytes programming{uds::kSidDiagnosticSessionControl};
    if (*connected_03)
    {
        programming.push_back(0x02);
    }
    if (*connected_43)
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

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    // upload_kernel(), revision 59f4e442:517-782. The unusual <= loop is
    // intentional: after every 128-byte payload block the oracle emits one
    // final address-only 0xB6 request.
    const std::size_t padded_to_word = (kernel.bytes.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_to_word + kKernelUploadBlockSize - 1U) / kKernelUploadBlockSize;
    if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / kKernelUploadBlockSize)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel kernel upload size is invalid");
    }
    const std::size_t data_length = block_count * kKernelUploadBlockSize;
    if (data_length > 0xFFFFFFU)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel kernel upload exceeds the 24-bit wire length");
    }
    bytes::Bytes plain = kernel.bytes;
    plain.resize(data_length, bytes::Byte{0});
    if (plain.size() < 4)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel kernel upload is shorter than its checksum word");
    }
    plain.resize(plain.size() - 4);
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < plain.size(); offset += 4)
    {
        sum += bytes::ReadU32Be(plain, offset);
    }
    bytes::AppendU32Be(plain, 0x5AA5A55AU - sum);
    const bytes::Bytes encrypted = EncryptPayload(plain);

    LogDebug(context, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    LogInfo(context, "Initialize kernel upload");
    if (const Status initialized = StrictPayload(context,
                                                 ComposeBe(0x34_b, 0x04_b, 0x33_b, bytes::U24(kernel.load_address),
                                                           bytes::U24(static_cast<std::uint32_t>(data_length))),
                                                 bytes::Bytes{0x20}, "kernel download", kKernelStartPolicy);
        !initialized.has_value())
    {
        return initialized;
    }

    LogInfo(context, "Uploading kernel, please wait...");
    std::size_t remaining = data_length;
    for (std::size_t block = 0; block <= block_count; ++block)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "kernel upload cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t block_offset = block * kKernelUploadBlockSize;
        const std::size_t chunk_size = block == block_count ? remaining : kKernelUploadBlockSize;
        const std::uint32_t address = kernel.load_address + static_cast<std::uint32_t>(block_offset);
        const bytes::ByteView chunk(encrypted.data() + static_cast<std::ptrdiff_t>(block_offset), chunk_size);
        const bytes::Bytes transfer = ComposeBe(0xB6_b, bytes::U24(address), chunk);
        if (const Status discarded = UploadB6Discard(context, transfer); !discarded.has_value())
        {
            return discarded;
        }
        if (block < block_count)
        {
            remaining -= kKernelUploadBlockSize;
        }
    }
    LogDebug(context, std::format("Data bytes sent: 0x{}", data_length));

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

Result<bytes::Bytes> ReadMemory(Context& context, const FlashPlan& plan, PhaseReporter& progress)
{
    // read_mem(), revision 59f4e442:789-936. Reads remain fixed at 0x400
    // bytes and proprietary BEEF replies are appended as received; the
    // legacy read path performs no payload decryption.
    const MemoryRegion region = plan.TransferRegion();
    bytes::Bytes rom;
    rom.reserve(region.length);
    LogInfo(context, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kKernelPageSize)
    {
        const auto started = context.clock.Now();
        if (const Status checkpoint = CancelledIfRequested(context, "read cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request = ComposeBe(bytes::Byte{0x00}, bytes::U24(address), std::uint16_t{kKernelPageSize});
        Result<bytes::Bytes> reply = BeefExchange(context, kKernelReadArea, request, kKernelPageSize, kReadTimeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        bytes::Bytes page(bytes::ByteView(*reply).first(kKernelPageSize).begin(),
                          bytes::ByteView(*reply).first(kKernelPageSize).end());
        if (page.size() > region.length - offset)
        {
            page.resize(region.length - offset);
        }

        // Local defect proof: revision-59f4e442:890 samples an unstarted
        // QElapsedTimer before line 891 starts it. A deterministic
        // monotonic sample and 1 ms minimum preserve the formula without an
        // invalid elapsed value; the exact first/last logs are regression
        // tested and disclosed in the diesel qualification row.
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(started, context.clock.Now());
        unsigned speed = static_cast<unsigned>(kKernelPageSize * (1000.0F / static_cast<float>(elapsed_ms)));
        if (speed == 0)
        {
            speed = 1;
        }
        const unsigned time_left = static_cast<unsigned>(((region.length - offset) / speed) % 9999U) + 1U;
        LogInfo(context, std::format("Kernel read addr: 0x{:08X} length: 0x{:08X}, {:>6} B/s {:>6} s", address,
                                     kKernelPageSize, speed, time_left));
        rom.insert(rom.end(), page.begin(), page.end());
        progress.Update(static_cast<int>(std::min<std::uint64_t>(region.length, offset + kKernelPageSize)));
    }
    LogInfo(context, "ROM read ready");
    return rom;
}

struct CompareResult
{
    std::vector<bool> modified;
    std::size_t changed_count{};
};

Result<std::uint32_t> QueryCrc(Context& context, const MemoryRegion& block)
{
    // check_romcrc(), revision 59f4e442:1101-1187.
    const bytes::Bytes payload = ComposeBe(block.start, bytes::Byte{0x00}, bytes::U24(block.length));
    Result<bytes::Bytes> reply = BeefExchange(context, kKernelCrc, payload, 4, kExtraLongTimeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const std::uint32_t crc = bytes::ReadU32Be(*reply);
    if (const Status drained = DiscardStaleFrame(context); !drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

Result<CompareResult> CompareBlocks(Context& context, const FlashPlan& plan, bytes::ByteView image,
                                    PhaseReporter *progress, bool after_reflash = false)
{
    // write_mem()/get_changed_blocks(), revision 59f4e442:943-1093.
    CompareResult result;
    result.modified.assign(plan.EraseRegions().size(), false);
    LogInfo(context, after_reflash ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                                   : "--- Comparing ECU flash memory pages to image file ---");
    LogInfo(context, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < plan.EraseRegions().size(); ++index)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "ROM compare cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const MemoryRegion block = plan.EraseRegions()[index];
        if (block.start < plan.TransferRegion().start ||
            static_cast<std::uint64_t>(block.start - plan.TransferRegion().start) + block.length > image.size())
        {
            return Fail(ErrorKind::kInvalidConfig, "diesel CRC block is outside the image");
        }
        LogInfo(context, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        Result<std::uint32_t> ecu_crc = QueryCrc(context, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::size_t image_offset = block.start - plan.TransferRegion().start;
        const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(image_offset, block.length));
        LogDebug(context, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
        LogInfo(context, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
        result.modified[index] = *ecu_crc != image_crc;
        LogInfo(context, result.modified[index] ? "\tNO" : "\tYES");
        if (result.modified[index])
        {
            ++result.changed_count;
        }
        if (progress != nullptr)
        {
            progress->Update(static_cast<int>(index + 1));
        }
        if (const Status slept = context.clock.Sleep(5ms, context.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    LogInfo(context, "Different blocks : ");
    for (std::size_t index = 0; index < result.modified.size(); ++index)
    {
        if (result.modified[index])
        {
            LogInfo(context, std::format("{}, ", index));
        }
    }
    LogInfo(context, std::format(" (total: {})", result.changed_count));
    return result;
}

Status InitializeFlash(Context& context, bool test_write)
{
    // init_flash_write(), revision 59f4e442:1189-1349.
    LogInfo(context, "Check max message length");
    Result<bytes::Bytes> max_message = BeefExchange(context, kKernelGetMaxMessage, {}, 4, kMediumTimeout);
    if (!max_message.has_value())
    {
        return std::unexpected(max_message.error());
    }
    LogInfo(context, std::format(": 0x{:04x}", bytes::ReadU32Be(*max_message)));

    LogInfo(context, "Check flashblock size");
    Result<bytes::Bytes> max_block = BeefExchange(context, kKernelGetMaxBlock, {}, 4, kMediumTimeout);
    if (!max_block.has_value())
    {
        return std::unexpected(max_block.error());
    }
    LogInfo(context, std::format(": 0x{:04x}", bytes::ReadU32Be(*max_block)));

    const bytes::Byte command = test_write ? kKernelFlashDisable : kKernelFlashEnable;
    LogInfo(context, test_write ? "Test write mode on, no actual flash write is performed"
                                : "Test write mode off, perform actual flash write");
    Result<bytes::Bytes> mode = BeefExchange(context, command, {}, 0, kMediumTimeout);
    if (!mode.has_value())
    {
        return std::unexpected(mode.error());
    }
    LogError(context, "Flash mode succesfully set");
    return {};
}

Status FlashBlock(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                  bool test_write, std::size_t& flash_bytes_index, std::size_t flash_bytes_count,
                  PhaseReporter& progress)
{
    // flash_block(), revision 59f4e442:1445-1689. TestWrite deliberately
    // follows the same erase/buffer path, selecting validate (0x23) instead
    // of commit (0x24) at each 0x1000-byte boundary.
    if (block.length == 0 || block.length % kFlashBufferSize != 0 || block.length % kFlashCommitSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel flash block is not aligned to write windows");
    }
    if (block.start < plan.TransferRegion().start)
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel flash block starts before the image");
    }
    const std::size_t image_offset = block.start - plan.TransferRegion().start;
    if (image_offset + block.length > image.size())
    {
        return Fail(ErrorKind::kInvalidConfig, "diesel flash block extends beyond the image");
    }

    LogInfo(context, std::format("Flash page erase addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    LogInfo(context, "Erasing flash page...");
    Result<bytes::Bytes> erased = BeefExchange(context, kKernelBlankPage, ComposeBe(block.start), 0, kExtraLongTimeout);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    LogInfo(context, " erased");

    auto previous_time = context.clock.Now();
    std::uint32_t address = block.start;
    std::uint32_t remaining = block.length;
    std::uint32_t commit_start = block.start;
    LogInfo(context, std::format("Start flash write addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    while (remaining != 0)
    {
        if (const Status checkpoint = CancelledIfRequested(context, "flash write cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t chunk_offset = static_cast<std::size_t>(address - block.start);
        const bytes::ByteView chunk = image.subspan(image_offset + chunk_offset, kFlashBufferSize);
        Result<bytes::Bytes> written =
            BeefExchange(context, kKernelWriteBuffer, ComposeBe(address, chunk), 0, kLongTimeout);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        LogDebug(context, "Data written to flash buffer");

        // Local defect proof: revision-59f4e442:1462 declares curspeed and
        // tleft uninitialized; lines 1578-1585 format them before assignments
        // at lines 1599 and 1605. Sample IClock first, clamp 0 ms to
        // 1, and otherwise retain the exact legacy formulas and log shape.
        const std::uint32_t percent = static_cast<std::uint32_t>(100U * (block.length - remaining) / block.length);
        const auto now = context.clock.Now();
        const std::uint64_t elapsed = ElapsedMilliseconds(previous_time, now);
        previous_time = now;
        std::uint32_t speed = static_cast<std::uint32_t>(kFlashBufferSize * 1000U / elapsed);
        if (speed == 0)
        {
            speed = 1;
        }
        const std::size_t next_index = flash_bytes_index + kFlashBufferSize;
        std::uint32_t time_left = static_cast<std::uint32_t>((flash_bytes_count - next_index) / speed);
        if (time_left > 9999)
        {
            time_left = 9999;
        }
        ++time_left;
        LogInfo(context,
                std::format("Write flash buffer: 0x{:08X} ({}% - {} B/s, ~ {} s)", address, percent, speed, time_left));

        remaining -= kFlashBufferSize;
        address += kFlashBufferSize;
        flash_bytes_index += kFlashBufferSize;
        progress.Update(static_cast<int>(std::min(flash_bytes_index, flash_bytes_count)));

        if (address - commit_start == kFlashCommitSize)
        {
            if (const Status checkpoint = CancelledIfRequested(context, "flash commit cancelled");
                !checkpoint.has_value())
            {
                return checkpoint;
            }
            LogInfo(context, "Flash buffer write complete... ");
            const std::size_t crc_offset = image_offset + static_cast<std::size_t>(commit_start - block.start);
            const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(crc_offset, kFlashCommitSize));
            LogDebug(context, std::format("Image CRC32: 0x{:x}", image_crc));
            if (test_write)
            {
                LogInfo(context, std::format("Validate flash addr: 0x{:x}", commit_start));
            }
            else
            {
                LogInfo(context, std::format("Committ flash addr: 0x{:x}", commit_start));
            }
            LogInfo(context, std::format(" len: 0x{:x}", kFlashCommitSize));
            LogInfo(context, std::format(" crc32: 0x{:x}", image_crc));
            const bytes::Bytes commit = ComposeBe(commit_start, std::uint16_t{kFlashCommitSize}, image_crc);
            Result<bytes::Bytes> committed = BeefExchange(
                context, test_write ? kKernelValidateBuffer : kKernelCommitBuffer, commit, 0, kExtraLongTimeout);
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            commit_start += kFlashCommitSize;
        }
    }
    return {};
}

Status ReflashBlock(Context& context, bytes::ByteView image, const FlashPlan& plan, const MemoryRegion& block,
                    bool test_write, std::size_t& flash_bytes_index, std::size_t flash_bytes_count,
                    PhaseReporter& progress)
{
    // reflash_block(), revision 59f4e442:1351-1443.
    LogInfo(context, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
    LogInfo(context, "Check flash voltage");
    Result<bytes::Bytes> voltage = BeefExchange(context, kKernelProgVolt, {}, 2, kMediumTimeout);
    if (!voltage.has_value())
    {
        return std::unexpected(voltage.error());
    }
    const double volts = static_cast<double>(bytes::ReadU16Be(*voltage)) / 50.0;
    LogInfo(context, std::format(": {}V", volts));

    Status flashed =
        FlashBlock(context, image, plan, block, test_write, flash_bytes_index, flash_bytes_count, progress);
    if (!flashed.has_value())
    {
        LogError(context,
                 "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely still "
                 "running and receiving commands!");
        return flashed;
    }
    LogInfo(context, "Flash block ok");
    return {};
}

Status WriteMemory(Context& context, const FlashPlan& plan, PhaseSequence& phases, PhaseReporter& compare_progress)
{
    // write_mem(), revision 59f4e442:943-1057.
    const bytes::ByteView image = plan.ImageOrEmpty();
    Result<CompareResult> before = CompareBlocks(context, plan, image, &compare_progress);
    if (!before.has_value())
    {
        return std::unexpected(before.error());
    }
    compare_progress.Complete();
    if (before->changed_count == 0)
    {
        LogInfo(context, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        phases.Start("Write", 0);
        return {};
    }

    std::size_t flash_bytes_count = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (before->modified[index])
        {
            flash_bytes_count += plan.EraseRegions()[index].length;
        }
    }
    LogInfo(context, "--- Start writing ROM file to ECU flash memory ---");
    const bool test_write = plan.Operation() == FlashOperation::kTestWrite;
    if (const Status initialized = InitializeFlash(context, test_write); !initialized.has_value())
    {
        return initialized;
    }

    PhaseReporter write_progress = phases.Start("Write", static_cast<int>(flash_bytes_count));
    std::size_t flash_bytes_index = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (!before->modified[index])
        {
            continue;
        }
        if (const Status checkpoint = CancelledIfRequested(context, "flash block loop cancelled");
            !checkpoint.has_value())
        {
            return checkpoint;
        }
        const Status reflashed = ReflashBlock(context, image, plan, plan.EraseRegions()[index], test_write,
                                              flash_bytes_index, flash_bytes_count, write_progress);
        if (!reflashed.has_value())
        {
            LogInfo(context, std::format("Block {} reflash failed.", index));
            return reflashed;
        }
        LogInfo(context, std::format("Block {} reflash complete.", index));
    }
    write_progress.Complete();

    Result<CompareResult> after = CompareBlocks(context, plan, image, nullptr, true);
    if (!after.has_value())
    {
        return std::unexpected(after.error());
    }
    if (test_write)
    {
        LogInfo(context, "*** Test write PASS, it's ok to perform actual write! ***");
    }
    else if (after->changed_count != 0)
    {
        LogError(context, "*** ERROR IN FLASH PROCESS ***");
        LogError(context, "Don't power off your ECU, kernel is still running and you can try flashing again!");
    }
    return {};
}

} // namespace

Result<Iso15765Config> SubaruDensoSh7058CanDieselExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh7058CanDiesel); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh7058CanDieselPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return Iso15765ConfigFrom(std::get<SubaruDensoSh7058CanDieselPlan>(plan.FamilyPlan()));
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

Result<FlashExecutionResult> SubaruDensoSh7058CanDieselExecutor::Execute(const FlashPlan& plan,
                                                                         ICanFlashTransport& transport, IClock& clock,
                                                                         const ICancellationToken& cancellation,
                                                                         IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh7058CanDiesel); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh7058CanDieselPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDensoSh7058CanDieselPlan>(plan.FamilyPlan());
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{cancellation, events, clock, transport, family.request_id, family.response_id, uds_client, channel};

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
        Result<bytes::Bytes> rom = ReadMemory(context, plan, read_phase);
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
    if (const Status written = WriteMemory(context, plan, phases, compare_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    PhaseReporter complete = phases.Start("Complete", 1);
    complete.Complete();
    return FlashExecutionResult{.operation = plan.Operation()};
}

} // namespace fastecu::flash
