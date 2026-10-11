#include "src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h"

#include <algorithm>
#include <array>
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
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/transfer_progress.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below is transcribed from revision 59f4e442 of
// src/platform/desktop/common/flash/legacy/ecu/
// flash_ecu_subaru_denso_sh7058_can_operation.cpp: connect_bootloader
// (112-621), upload_kernel (626-870), read_mem (875-1022), write_mem
// (1027-1143), CRC/init/reflash/flash (1148-1760), security (1765-1906),
// payload crypto (1911-1940), and request_kernel_id (1946-1996).
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

constexpr std::array<std::uint16_t, 16> kCobbSeedTable{
    0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092,
    0x9E05, 0x8723, 0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9,
};
constexpr std::array<std::uint8_t, 32> kEcuTekIndexTransformation{
    0x04, 0x02, 0x05, 0x01, 0x08, 0x0C, 0x0D, 0x08, 0x0A, 0x0D, 0x02, 0x0B, 0x0F, 0x04, 0x00, 0x03,
    0x0B, 0x04, 0x06, 0x00, 0x0F, 0x02, 0x0D, 0x09, 0x05, 0x0C, 0x01, 0x0A, 0x03, 0x0D, 0x0E, 0x08,
};
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
    SubaruDensoSh7058CanSecurity security;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
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

Result<bytes::Bytes> ChannelRequest(Context& context, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                                    std::chrono::milliseconds delay = 0ms)
{
    Result<std::optional<bytes::Bytes>> received = ChannelRequestOptional(context, pdu, timeout, delay);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no CAN response within the read timeout");
    }
    return std::move(**received);
}

Status UploadB6Discard(Context& context, bytes::ByteView pdu)
{
    // upload_kernel(), revision 59f4e442:738-751 unconditionally reads and
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
    const Result<std::optional<bytes::Bytes>> ignored = context.transport.Read(kReadTimeout, context.cancellation);
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
    Result<bytes::Bytes> reply = ChannelRequest(context, BeefRequest(opcode, payload), timeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    Result<BeefMessage> parsed = ParseBeef(*reply);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const bytes::Byte expected = static_cast<bytes::Byte>(opcode | 0x40U);
    if (parsed->opcode != expected)
    {
        return Fail(ErrorKind::kBadResponse, std::format("unexpected BEEF response opcode 0x{:02x}, expected 0x{:02x}",
                                                         parsed->opcode, expected));
    }
    if (parsed->payload.size() < minimum_payload)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response payload");
    }
    return bytes::Bytes(parsed->payload.begin(), parsed->payload.end());
}

Status DiscardStaleFrame(Context& context)
{
    // check_romcrc(), revision 59f4e442:1260-1266 deliberately ignores this
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
        LogError(context, received.error().kind == ErrorKind::kTimeout ? "No valid response from ECU"
                                                                       : "Wrong response from ECU");
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        LogError(context, "No valid response from ECU");
    }
    return received;
}

Status StrictPayload(Context& context, bytes::ByteView pdu, bytes::ByteView expected, std::string_view label,
                     const uds::ExchangePolicy& policy = kStrictPolicy)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, policy, context.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.size() < expected.size() || !std::equal(expected.begin(), expected.end(), payload.begin()))
    {
        LogError(context, std::format("Wrong response from ECU during {}", label));
        return Fail(ErrorKind::kBadResponse, std::format("unexpected response during {}", label));
    }
    return {};
}

std::uint32_t ModularPower(std::uint32_t base, std::uint32_t exponent, std::uint32_t modulus)
{
    std::uint64_t result = 1;
    std::uint64_t factor = base % modulus;
    while (exponent != 0)
    {
        if ((exponent & 1U) != 0)
        {
            result = (result * factor) % modulus;
        }
        factor = (factor * factor) % modulus;
        exponent >>= 1U;
    }
    return static_cast<std::uint32_t>(result);
}

struct RaceRomAltValues
{
    std::uint32_t seed_alter{};
    bytes::Byte xor_byte_1{};
    bytes::Byte xor_byte_2{};
};

Result<bytes::Bytes> SecurityKey(Context& context, bytes::ByteView seed, const std::optional<RaceRomAltValues>& alt)
{
    if (seed.size() != 4)
    {
        return Fail(ErrorKind::kBadResponse, "petrol security seed must contain four bytes");
    }
    switch (context.security)
    {
    case SubaruDensoSh7058CanSecurity::kStock:
        LogInfo(context, "Using stock seed key algo");
        return DensoSeedKey(seed);
    case SubaruDensoSh7058CanSecurity::kEcuTek:
        LogInfo(context, "Using EcuTek seed key algo");
        return ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, kEcuTekIndexTransformation);
    case SubaruDensoSh7058CanSecurity::kRaceRom:
    {
        LogInfo(context, "Using EcuTek RaceRom RSA algo");
        const std::uint32_t raw = bytes::ReadU32Be(seed);
        const std::uint32_t key = ModularPower(raw, 0x0A863281U, 0x0FDA9293U);
        LogInfo(context, std::format("Seed key: 0x{:08x}", key));
        return ComposeBe(key);
    }
    case SubaruDensoSh7058CanSecurity::kRaceRomAlt:
    {
        if (!alt.has_value())
        {
            return Fail(ErrorKind::kBadResponse, "RaceRom-alt RAM values are unavailable");
        }
        LogInfo(context, "Using EcuTek seed key algo");
        bytes::Bytes key = ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, kEcuTekIndexTransformation);
        std::uint32_t altered = bytes::ReadU32Be(key);
        constexpr std::uint32_t kXorMultiplier = 0x01000193U;
        altered = ((alt->seed_alter ^ altered) ^ alt->xor_byte_1) * kXorMultiplier;
        altered = (altered ^ alt->xor_byte_2) * kXorMultiplier;
        key = ComposeBe(altered);
        LogDebug(context, std::format("Altered seed key: {}", bytes::ToHex(key)));
        return key;
    }
    case SubaruDensoSh7058CanSecurity::kCobb:
        LogInfo(context, "Using COBB seed key algo");
        return ssm_protocol::CalculateSeedKey(seed, kCobbSeedTable, ssm_protocol::kIndexTransformationStock);
    }
    return Fail(ErrorKind::kInternal, "unknown petrol security variant");
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed)
{
    // request_kernel_id(), revision 59f4e442:1946-1980. The three trailing
    // zero bytes are outside the declared BEEF body but are present on the
    // legacy wire and therefore remain literal here.
    const bytes::Bytes request{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
    bytes::Bytes pdu;
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
            LogError(context, "Wrong response from ECU while requesting kernel ID");
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
        if (frame.size() < CanFlashUdsChannel::kEnvelopeSize || bytes::ReadU32Be(frame) != context.response_id)
        {
            LogError(context, "Wrong response from ECU while requesting kernel ID");
            return std::optional<std::string>{};
        }
        pdu.assign(frame.begin() + static_cast<std::ptrdiff_t>(CanFlashUdsChannel::kEnvelopeSize), frame.end());
    }
    else
    {
        Result<std::optional<bytes::Bytes>> reply =
            ChannelRequestOptional(context, request, kLongTimeout, kKernelProbeDelay);
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
        pdu = std::move(**reply);
    }

    Result<BeefMessage> parsed = ParseBeef(pdu);
    if (!parsed.has_value())
    {
        if (tolerate_malformed)
        {
            LogError(context, "Wrong response from ECU while requesting kernel ID");
            return std::optional<std::string>{};
        }
        return std::unexpected(parsed.error());
    }
    if (parsed->opcode != static_cast<bytes::Byte>(kKernelId | 0x40U))
    {
        if (tolerate_malformed)
        {
            LogError(context, "Wrong response from ECU while requesting kernel ID");
            return std::optional<std::string>{};
        }
        return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
    }
    return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
}

Result<std::uint32_t> ReadRamLocation(Context& context, std::uint32_t address)
{
    // connect_bootloader(), revision 59f4e442:558-618. The vendor request
    // carries four consecutive 24-bit addresses rather than a range length.
    LogDebug(context, std::format("Reading RAM value at location: 0x{:x}", address));
    bytes::Bytes request{0xA8, 0x00};
    for (std::uint32_t offset = 0; offset < 4; ++offset)
    {
        const bytes::Bytes encoded = ComposeBe(bytes::U24(address + offset));
        request.insert(request.end(), encoded.begin(), encoded.end());
    }
    Result<bytes::Bytes> reply = ChannelRequest(context, request, kReadTimeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (reply->size() < 5 || (*reply)[0] != 0xE8)
    {
        return Fail(ErrorKind::kBadResponse, "invalid RaceRom-alt RAM response");
    }
    return bytes::ReadU32Be(*reply, 1);
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
        LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(pdu)));
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
    LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(pdu)));
    return false;
}

Status ConnectBootloader(Context& context, bool read_operation, bool& kernel_alive, std::optional<std::string>& rom_id)
{
    // connect_bootloader(), revision 59f4e442:112-621.
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = RequestKernelId(context, true);
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

    LogError(context, "No valid response from ECU");
    LogInfo(context, "No response from kernel, initialising ECU...");
    LogInfo(context, "Initialising connection...");

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
            LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(pdu)));
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
            LogError(context, std::format("Wrong response from ECU: {}", bytes::ToHex(pdu)));
        }
    }

    std::optional<RaceRomAltValues> race_rom_alt;
    if (context.security == SubaruDensoSh7058CanSecurity::kRaceRomAlt)
    {
        if (!cal_id->has_value())
        {
            LogInfo(context, "Unknown EcuTek ROM found... exiting...");
            return Fail(ErrorKind::kBadResponse, "RaceRom-alt CAL ID is unavailable");
        }
        std::uint32_t seed_address = 0;
        std::uint32_t xor_address = 0;
        if (**cal_id == "AE5Z500V")
        {
            seed_address = 0xFFFF1ED8;
            xor_address = 0xFFFF1E80;
        }
        else if (**cal_id == "AZ1G202I")
        {
            seed_address = 0xFFFFBBFC;
            xor_address = 0xFFFFBC00;
        }
        else if (**cal_id == "AE5I910V")
        {
            seed_address = 0xFFFFBBA8;
            xor_address = 0xFFFFBC00;
        }
        else
        {
            LogInfo(context, "Unknown EcuTek ROM found... exiting...");
            return Fail(ErrorKind::kBadResponse, "unsupported RaceRom-alt CAL ID");
        }
        Result<std::uint32_t> seed_alter = ReadRamLocation(context, seed_address);
        if (!seed_alter.has_value())
        {
            return std::unexpected(seed_alter.error());
        }
        LogDebug(context, std::format("Value at RAM loc 0x{:x} is: 0x{:x}", seed_address, *seed_alter));
        Result<std::uint32_t> xor_value = ReadRamLocation(context, xor_address);
        if (!xor_value.has_value())
        {
            return std::unexpected(xor_value.error());
        }
        LogDebug(context, std::format("Value at RAM loc 0x{:x} is: 0x{:x}", xor_address, *xor_value));
        const std::uint32_t xor_value_u32 = static_cast<std::uint32_t>(*xor_value);
        race_rom_alt = RaceRomAltValues{.seed_alter = *seed_alter,
                                        .xor_byte_1 = static_cast<bytes::Byte>((xor_value_u32 >> 8U) & 0xFFU),
                                        .xor_byte_2 = static_cast<bytes::Byte>(xor_value_u32 & 0xFFU)};
        LogInfo(context, std::format("Seed alter is: 0x{:x}", race_rom_alt->seed_alter));
        LogInfo(context,
                std::format("XOR values are: 0x{:x} and 0x{:x}", race_rom_alt->xor_byte_1, race_rom_alt->xor_byte_2));
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
    Result<bytes::Bytes> seed_reply = context.uds.Request(
        bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, kStrictPolicy, context.cancellation);
    if (!seed_reply.has_value())
    {
        return std::unexpected(seed_reply.error());
    }
    const bytes::ByteView seed_payload = uds::Payload(*seed_reply);
    if (seed_payload.size() < 5 || seed_payload[0] != uds::kSecurityAccessRequestSeed)
    {
        return Fail(ErrorKind::kBadResponse, "invalid petrol seed response");
    }
    LogInfo(context, "Seed request ok");
    Result<bytes::Bytes> key = SecurityKey(context, seed_payload.subspan(1, 4), race_rom_alt);
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
    Result<bytes::Bytes> programming_reply = context.uds.Request(programming, kStrictPolicy, context.cancellation);
    if (!programming_reply.has_value())
    {
        return std::unexpected(programming_reply.error());
    }
    const bytes::ByteView programming_payload = uds::Payload(*programming_reply);
    if (programming_payload.empty() || (programming_payload[0] != 0x02 && programming_payload[0] != 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(context, "Succesfully set to programming session");
    return {};
}

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    // upload_kernel(), revision 59f4e442:626-868. The unusual <= loop is
    // intentional: after every 128-byte payload block the oracle emits one
    // final address-only 0xB6 request.
    const std::size_t padded_to_word = (kernel.bytes.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_to_word + kKernelUploadBlockSize - 1U) / kKernelUploadBlockSize;
    if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / kKernelUploadBlockSize)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol kernel upload size is invalid");
    }
    const std::size_t data_length = block_count * kKernelUploadBlockSize;
    if (data_length > 0xFFFFFFU)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol kernel upload exceeds the 24-bit wire length");
    }
    bytes::Bytes plain = kernel.bytes;
    plain.resize(data_length, bytes::Byte{0});
    if (plain.size() < 4)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol kernel upload is shorter than its checksum word");
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
                                                 bytes::Bytes{0x20}, "kernel download");
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

    LogInfo(context, "Kernel uploaded, starting...");
    if (const Status exited =
            StrictPayload(context, bytes::Bytes{uds::kSidRequestTransferExit}, {}, "kernel transfer exit");
        !exited.has_value())
    {
        return exited;
    }
    if (const Status delay = context.clock.Sleep(kKernelStartDelay, context.cancellation); !delay.has_value())
    {
        return delay;
    }
    // upload_kernel(), revision 59f4e442 lines 815-835 checks only the
    // positive service id, so an echoed routine is optional.
    if (const Status started = StrictPayload(context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02}, {},
                                             "kernel start", kKernelStartPolicy);
        !started.has_value())
    {
        return started;
    }

    LogInfo(context, "Kernel requesting kernel ID...");
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
    // read_mem(), revision 59f4e442:875-1019. Reads remain fixed at 0x400
    // bytes and append each proprietary BEEF page payload unchanged.
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
        const bytes::ByteView raw_page = bytes::ByteView(*reply).first(kKernelPageSize);
        bytes::Bytes page(raw_page.begin(), raw_page.end());
        if (page.size() > region.length - offset)
        {
            page.resize(region.length - offset);
        }

        // Local defect proof: revision-59f4e442:877 declares an unstarted
        // QElapsedTimer and line 974 samples it before start. A deterministic
        // monotonic sample and 1 ms minimum preserve the formula without an
        // invalid elapsed value; the exact first/last logs are regression
        // tested and disclosed in the petrol qualification row.
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(started, context.clock.Now());
        const TransferRate rate = ComputeTransferRate(kKernelPageSize, elapsed_ms, region.length - offset);
        LogInfo(context, FormatReadProgress(address, kKernelPageSize, rate));
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
    // check_romcrc(), revision 59f4e442:1185-1267.
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
    // write_mem()/get_changed_blocks(), revision 59f4e442:1027-1177.
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
            return Fail(ErrorKind::kInvalidConfig, "petrol CRC block is outside the image");
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
    // init_flash_write(), revision 59f4e442:1270-1418.
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
    // flash_block(), revision 59f4e442:1518-1757. TestWrite deliberately
    // follows the same erase/buffer path, selecting validate (0x23) instead
    // of commit (0x24) at each 0x1000-byte boundary.
    if (block.length == 0 || block.length % kFlashBufferSize != 0 || block.length % kFlashCommitSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol flash block is not aligned to write windows");
    }
    if (block.start < plan.TransferRegion().start)
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol flash block starts before the image");
    }
    const std::size_t image_offset = block.start - plan.TransferRegion().start;
    if (image_offset + block.length > image.size())
    {
        return Fail(ErrorKind::kInvalidConfig, "petrol flash block extends beyond the image");
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

        // Local defect proof: revision-59f4e442:1535-1536 leaves curspeed
        // and tleft uninitialized, then formats them at 1651-1655 before the
        // first assignment at 1664-1682. Sample IClock first, clamp 0 ms to
        // 1, and otherwise retain the exact legacy formulas and log shape.
        const std::uint32_t percent = static_cast<std::uint32_t>(100U * (block.length - remaining) / block.length);
        const auto now = context.clock.Now();
        const std::uint64_t elapsed = ElapsedMilliseconds(previous_time, now);
        previous_time = now;
        const auto next_index = flash_bytes_index + kFlashBufferSize;
        const TransferRate rate = ComputeTransferRate(kFlashBufferSize, elapsed, flash_bytes_count - next_index);
        LogInfo(context, FormatWriteProgress(address, percent, rate));

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
    // reflash_block(), revision 59f4e442:1426-1510.
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
    // write_mem(), revision 59f4e442:1027-1140.
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

Result<Iso15765Config> SubaruDensoSh7058CanExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh7058Can); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh7058CanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return Iso15765ConfigFrom(std::get<SubaruDensoSh7058CanPlan>(plan.FamilyPlan()));
}

Status SubaruDensoSh7058CanExecutor::BeforeTransportConfigure(ICanFlashTransport& transport, IClock&,
                                                              const ICancellationToken& cancellation) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before petrol CAN reset");
    }
    if (const Status reset = transport.ResetConnection(); !reset)
    {
        return reset;
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after petrol CAN reset");
    }
    return {};
}

Status SubaruDensoSh7058CanExecutor::BeforeTransportOpen(const ICancellationToken& cancellation) const
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after petrol CAN configuration");
    }
    return {};
}

Result<FlashExecutionResult> SubaruDensoSh7058CanExecutor::Execute(const FlashPlan& plan, ICanFlashTransport& transport,
                                                                   IClock& clock,
                                                                   const ICancellationToken& cancellation,
                                                                   IEventSink& events)
{
    if (const Status matched = CheckFamily(plan, FlashFamily::kSubaruDensoSh7058Can); !matched.has_value())
    {
        return std::unexpected(matched.error());
    }
    if (const Status valid = ValidateSubaruDensoSh7058CanPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }

    const auto& family = std::get<SubaruDensoSh7058CanPlan>(plan.FamilyPlan());
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{cancellation,       events,          clock,      transport, family.request_id,
                    family.response_id, family.security, uds_client, channel};

    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    bool kernel_alive = false;
    std::optional<std::string> rom_id;
    if (const Status connected = ConnectBootloader(context, read_operation, kernel_alive, rom_id);
        !connected.has_value())
    {
        return std::unexpected(connected.error());
    }
    if (!kernel_alive)
    {
        events.Notice("Preparing, please wait...");
        if (const Status uploaded = UploadKernel(context, plan.KernelOrEmpty()); !uploaded.has_value())
        {
            return std::unexpected(uploaded.error());
        }
    }
    kernel_phase.Complete();

    if (read_operation)
    {
        events.Notice("Reading ROM, please wait...");
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
