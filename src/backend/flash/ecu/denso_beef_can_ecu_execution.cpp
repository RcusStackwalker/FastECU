#include "src/backend/flash/ecu/denso_beef_can_ecu_execution.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_diesel_types.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_types.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/beef_can_kernel.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/protocol/uds/uds_client.h"

// Every exchange below is transcribed from the legacy
// flash_ecu_subaru_denso_sh7058_can_diesel_operation.cpp (and, for petrol,
// flash_ecu_subaru_denso_sh7058_can_operation.cpp). Petrol's five security variants and RaceRom-alt RAM preparation
// remain selected solely by the admitted petrol plan.
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
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
    const DensoBeefCanProfile& profile;
    BeefCanKernel kernel;
};

std::string UppercaseHexCompact(bytes::ByteView data)
{
    std::string rendered;
    for (const bytes::Byte value : data)
    {
        std::format_to(std::back_inserter(rendered), "{:02X}", value);
    }
    return rendered;
}

Status CancelledIfRequested(const Context& ctx, std::string_view detail)
{
    if (ctx.cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::string(detail));
    }
    return {};
}

// Sends `pdu` and returns whatever arrives within `timeout`.
Result<std::optional<bytes::Bytes>>
ChannelRequestOptional(Context& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    return CancelledIfRequested(ctx, "cancelled before CAN request")
        .and_then([&] { return ctx.channel.Send(pdu, ctx.cancellation); })
        .and_then(
            [&] { return delay > std::chrono::milliseconds{0} ? ctx.clock.Sleep(delay, ctx.cancellation) : Status{}; })
        .and_then([&] { return ctx.channel.Receive(timeout, ctx.cancellation); });
}

constexpr std::chrono::milliseconds kNonfatalReadTimeout{2000};
constexpr std::chrono::milliseconds kNonfatalSendDelay{50};

// request_kernel_id(): the three trailing zero bytes are outside the declared
// BEEF body but are present on the legacy wire, so they stay literal.
constexpr bytes::Byte kKernelId = 0x01;
constexpr std::array<bytes::Byte, 8> kKernelIdRequest{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
constexpr std::chrono::milliseconds kKernelIdTimeout{800};
constexpr std::chrono::milliseconds kKernelIdDelay{200};

Result<std::string> ParseKernelId(bytes::ByteView pdu)
{
    if (pdu.size() < 5)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response envelope");
    }
    if (bytes::ReadU16Be(pdu) != 0xBEEF)
    {
        return Fail(ErrorKind::kBadResponse, "wrong BEEF response marker");
    }
    const auto declared = bytes::ReadU16Be(pdu, 2);
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > pdu.size())
    {
        return Fail(ErrorKind::kBadResponse, "invalid BEEF response length");
    }
    if (pdu[4] != static_cast<bytes::Byte>(kKernelId | 0x40U))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
    }
    return std::string(pdu.begin() + 5, pdu.begin() + declared + 4);
}

void LogWrongPdu(const Context& context, bytes::ByteView pdu)
{
    const std::string rendered =
        context.profile.pdu_format == BeefPduFormat::kHex ? bytes::ToHex(pdu) : uds::Describe(pdu);
    LogError(context, std::format("Wrong response from {}: {}", context.profile.device, rendered));
}

void LogFailure(const Context& context, const Error& failure)
{
    if (failure.kind == ErrorKind::kTimeout)
    {
        LogError(context, "No valid response from ECU");
    }
    else if (failure.kind == ErrorKind::kBadResponse)
    {
        LogError(context, std::format("Wrong response from {}: {}", context.profile.device, failure.detail));
    }
}

Result<std::optional<bytes::Bytes>> NonfatalQuery(Context& context, bytes::ByteView pdu)
{
    Result<std::optional<bytes::Bytes>> received =
        ChannelRequestOptional(context, pdu, kNonfatalReadTimeout, kNonfatalSendDelay);
    if (!received.has_value())
    {
        const Error& failure = received.error();
        if (failure.kind == ErrorKind::kCancelled || failure.kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(failure);
        }
        LogError(context, failure.kind == ErrorKind::kTimeout
                              ? "No valid response from ECU"
                              : std::format("Wrong response from {}: {}", context.profile.device, failure.detail));
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        LogError(context, "No valid response from ECU");
    }
    return received;
}

Result<bytes::Bytes> RequestBootloaderSessions(Context& context)
{
    LogInfo(context, "Requesting session mode");
    bytes::Bytes programming{uds::kSidDiagnosticSessionControl};
    for (const bytes::Byte session : std::array<bytes::Byte, 2>{0x03, 0x43})
    {
        const auto reply = NonfatalQuery(context, bytes::Bytes{uds::kSidDiagnosticSessionControl, session});
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (!reply->has_value())
        {
            continue;
        }
        const auto& pdu = **reply;
        if (pdu.size() >= 2 && pdu[0] == 0x50 && pdu[1] == session)
        {
            programming.push_back(static_cast<bytes::Byte>(session - 1));
        }
        else
        {
            LogWrongPdu(context, pdu);
        }
    }
    return programming;
}

Result<std::optional<std::string>> RunningKernelId(Context& context)
{
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(context, kKernelIdRequest, kKernelIdTimeout, kKernelIdDelay);
    if (!reply.has_value() && reply.error().kind == ErrorKind::kDisconnected)
    {
        return std::unexpected(reply.error());
    }
    if (reply.has_value() && !reply->has_value())
    {
        return std::nullopt;
    }
    return CancelledIfRequested(context, "kernel ID probe cancelled")
        .transform(
            [&]() -> std::optional<std::string>
            {
                Result<std::string> id =
                    reply.and_then([](const std::optional<bytes::Bytes>& pdu) { return ParseKernelId(*pdu); });
                if (!id.has_value())
                {
                    LogFailure(context, id.error());
                    return std::nullopt;
                }
                return *id;
            });
}

Status StartedKernelId(Context& context)
{
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(context, kKernelIdRequest, kKernelIdTimeout, kKernelIdDelay);
    if (!reply.has_value() && reply.error().kind != ErrorKind::kTimeout)
    {
        LogFailure(context, reply.error());
        return std::unexpected(reply.error());
    }
    if (!reply.has_value() || !reply->has_value())
    {
        LogError(context, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "kernel did not answer after upload");
    }
    Result<std::string> id = ParseKernelId(**reply);
    if (!id.has_value())
    {
        LogFailure(context, id.error());
        return std::unexpected(id.error());
    }
    LogInfo(context, std::format("Kernel ID: {}", *id));
    return {};
}

Status RequestEcuId(Context& context, bool read_operation, std::optional<std::string>& rom_id)
{
    LogInfo(context, "Requesting ECU ID");
    Result<std::optional<bytes::Bytes>> ecu_reply = NonfatalQuery(context, bytes::Bytes{0xAA});
    if (!ecu_reply.has_value())
    {
        return std::unexpected(ecu_reply.error());
    }
    if (!ecu_reply->has_value())
    {
        return {};
    }
    const bytes::Bytes& pdu = **ecu_reply;
    if (pdu.size() < 9 || pdu[0] != 0xEA)
    {
        LogWrongPdu(context, pdu);
        return {};
    }
    const std::string ecu_id = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}", pdu[4], pdu[5], pdu[6], pdu[7], pdu[8]);
    LogInfo(context, std::format("ECU ID: {}", ecu_id));
    if (read_operation)
    {
        rom_id = ecu_id + "_";
    }
    return {};
}

// UdsClient requires the positive service id; neither ECU requires a payload
// echo for transfer exit or routine start. Diesel additionally logs failures.
template <typename Family>
Status StrictService(Context& context, bytes::ByteView pdu, const uds::ExchangePolicy& policy)
{
    Result<bytes::Bytes> reply = context.uds.Request(pdu, policy, context.cancellation);
    if (!reply.has_value())
    {
        if constexpr (std::same_as<Family, SubaruDensoSh7058CanDieselPlan>)
        {
            LogFailure(context, reply.error());
        }
        return std::unexpected(reply.error());
    }
    return {};
}

constexpr std::array<std::uint16_t, 16> kCobbSeedTable{
    0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092,
    0x9E05, 0x8723, 0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9,
};

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

Result<std::uint32_t> ReadRamLocation(Context& context, std::uint32_t address)
{
    // The vendor request carries four consecutive 24-bit addresses rather than
    // a range length.
    LogDebug(context, std::format("Reading RAM value at location: 0x{:x}", address));
    bytes::Bytes request{0xA8, 0x00};
    for (std::uint32_t offset = 0; offset < 4; ++offset)
    {
        const auto encoded = bytes::ComposeBe(bytes::U24{address + offset});
        request.insert(request.end(), encoded.begin(), encoded.end());
    }
    auto reply = ChannelRequestOptional(context, request, kReadTimeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no CAN response within the read timeout");
    }
    const auto& pdu = **reply;
    if (pdu.size() < 5 || pdu[0] != 0xE8)
    {
        return Fail(ErrorKind::kBadResponse, "invalid RaceRom-alt RAM response");
    }
    return bytes::ReadU32Be(pdu, 1);
}

struct RaceRomAltAddresses
{
    std::string_view cal_id;
    std::uint32_t seed_address;
    std::uint32_t xor_address;
};

constexpr auto kRaceRomAltAddresses = std::to_array<RaceRomAltAddresses>({
    {"AE5Z500V", 0xFFFF1ED8, 0xFFFF1E80},
    {"AZ1G202I", 0xFFFFBBFC, 0xFFFFBC00},
    {"AE5I910V", 0xFFFFBBA8, 0xFFFFBC00},
});

// RaceRom-alt ROMs keep their seed alteration and key XOR bytes in RAM at
// addresses that depend on the CAL ID.
Result<RaceRomAltValues> ReadRaceRomAltValues(Context& context, const std::optional<std::string>& cal_id)
{
    if (!cal_id.has_value())
    {
        LogInfo(context, "Unknown EcuTek ROM found... exiting...");
        return Fail(ErrorKind::kBadResponse, "RaceRom-alt CAL ID is unavailable");
    }
    const auto addresses = std::ranges::find(kRaceRomAltAddresses, *cal_id, &RaceRomAltAddresses::cal_id);
    if (addresses == kRaceRomAltAddresses.end())
    {
        LogInfo(context, "Unknown EcuTek ROM found... exiting...");
        return Fail(ErrorKind::kBadResponse, "unsupported RaceRom-alt CAL ID");
    }
    return ReadRamLocation(context, addresses->seed_address)
        .and_then(
            [&](std::uint32_t seed_alter)
            {
                LogDebug(context,
                         std::format("Value at RAM loc 0x{:x} is: 0x{:x}", addresses->seed_address, seed_alter));
                return ReadRamLocation(context, addresses->xor_address)
                    .transform(
                        [&](std::uint32_t xor_value)
                        {
                            LogDebug(context, std::format("Value at RAM loc 0x{:x} is: 0x{:x}", addresses->xor_address,
                                                          xor_value));
                            const RaceRomAltValues values{.seed_alter = seed_alter,
                                                          .xor_byte_1 =
                                                              static_cast<bytes::Byte>((xor_value >> 8U) & 0xFFU),
                                                          .xor_byte_2 = static_cast<bytes::Byte>(xor_value & 0xFFU)};
                            LogInfo(context, std::format("Seed alter is: 0x{:x}", values.seed_alter));
                            LogInfo(context, std::format("XOR values are: 0x{:x} and 0x{:x}", values.xor_byte_1,
                                                         values.xor_byte_2));
                            return values;
                        });
            });
}

template <typename Family> struct EcuFamilyTraits;

template <> struct EcuFamilyTraits<SubaruDensoSh7058CanPlan>
{
    static constexpr std::string_view kUnit = "Subaru Denso SH7058 petrol";
    static constexpr const DensoBeefCanProfile& kProfile = kSubaruDensoSh7058CanBeefProfile;
    static constexpr BeefKernelTransferPolicy kTransferPolicy{kStrictPolicy, kReadTimeout};
    static constexpr uds::ExchangePolicy kTransferExitPolicy = kStrictPolicy;
    static constexpr std::string_view kInitializeConnection = "Initialising connection...";
    static constexpr std::string_view kRequestStartedKernelId = "Kernel requesting kernel ID...";

    using SecurityState = std::optional<RaceRomAltValues>;

    static Result<SecurityState> PrepareSecurity(Context& context, const SubaruDensoSh7058CanPlan& family,
                                                 const std::optional<std::string>& cal_id)
    {
        if (family.security != SubaruDensoSh7058CanSecurity::kRaceRomAlt)
        {
            return SecurityState{};
        }
        return ReadRaceRomAltValues(context, cal_id)
            .transform([](const RaceRomAltValues& values) { return SecurityState{values}; });
    }

    static Result<bytes::Bytes> SecurityKey(Context& context, const SubaruDensoSh7058CanPlan& family,
                                            const SecurityState& alt, std::span<const bytes::Byte, 4> seed)
    {
        switch (family.security)
        {
        case SubaruDensoSh7058CanSecurity::kStock:
            LogInfo(context, "Using stock seed key algo");
            return DensoSeedKey(seed);
        case SubaruDensoSh7058CanSecurity::kEcuTek:
            LogInfo(context, "Using EcuTek seed key algo");
            return ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, ssm_protocol::kEcutekPairTable);
        case SubaruDensoSh7058CanSecurity::kRaceRom:
        {
            LogInfo(context, "Using EcuTek RaceRom RSA algo");
            const std::uint32_t raw = bytes::ReadU32Be(seed);
            const std::uint32_t key = ModularPower(raw, 0x0A863281U, 0x0FDA9293U);
            LogInfo(context, std::format("Seed key: 0x{:08x}", key));
            return bytes::ComposeBeWithExtraCapacity(0, key);
        }
        case SubaruDensoSh7058CanSecurity::kRaceRomAlt:
        {
            if (!alt.has_value())
            {
                return Fail(ErrorKind::kBadResponse, "RaceRom-alt RAM values are unavailable");
            }
            LogInfo(context, "Using EcuTek seed key algo");
            bytes::Bytes key =
                ssm_protocol::CalculateSeedKey(seed, kDensoIso15765SeedKeyTable, ssm_protocol::kEcutekPairTable);
            std::uint32_t altered = bytes::ReadU32Be(key);
            constexpr std::uint32_t kXorMultiplier = 0x01000193U;
            altered = ((alt->seed_alter ^ altered) ^ alt->xor_byte_1) * kXorMultiplier;
            altered = (altered ^ alt->xor_byte_2) * kXorMultiplier;
            key = bytes::ComposeBeWithExtraCapacity(0, altered);
            LogDebug(context, std::format("Altered seed key: {}", bytes::ToHex(key)));
            return key;
        }
        case SubaruDensoSh7058CanSecurity::kCobb:
            LogInfo(context, "Using COBB seed key algo");
            return ssm_protocol::CalculateSeedKey(seed, kCobbSeedTable, ssm_protocol::kStockPairTable);
        }
        return Fail(ErrorKind::kInternal, "unknown petrol security variant");
    }
};

template <> struct EcuFamilyTraits<SubaruDensoSh7058CanDieselPlan>
{
    static constexpr std::string_view kUnit = "Subaru 07+ Diesel 32-bit";
    static constexpr const DensoBeefCanProfile& kProfile = kSubaruDensoSh7058CanDieselBeefProfile;
    static constexpr BeefKernelTransferPolicy kTransferPolicy{kKernelStartPolicy, std::chrono::milliseconds{500}};
    static constexpr uds::ExchangePolicy kTransferExitPolicy = kKernelStartPolicy;
    static constexpr std::string_view kInitializeConnection = "Initializing connection...";
    static constexpr std::string_view kRequestStartedKernelId = "Requesting kernel ID...";

    struct SecurityState
    {
    };

    static Result<SecurityState> PrepareSecurity(Context&, const SubaruDensoSh7058CanDieselPlan&,
                                                 const std::optional<std::string>&)
    {
        return SecurityState{};
    }

    static Result<bytes::Bytes> SecurityKey(Context&, const SubaruDensoSh7058CanDieselPlan&, const SecurityState&,
                                            std::span<const bytes::Byte, 4> seed)
    {
        return DensoSeedKey(seed);
    }
};

Status EnterProgrammingSession(Context& ctx, bytes::ByteView request)
{
    LogInfo(ctx, "Set session mode");
    Result<bytes::Bytes> reply = ctx.uds.Request(request, kStrictPolicy, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.empty() || (payload[0] != 0x02 && payload[0] != 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(ctx, "Succesfully set to programming session");
    return {};
}

template <typename Family>
Status UnlockSecurity(Context& ctx, const Family& family,
                      const typename EcuFamilyTraits<Family>::SecurityState& security)
{
    const auto strict = [&](const bytes::Bytes& pdu)
    {
        Result<bytes::Bytes> reply = ctx.uds.Request(pdu, kStrictPolicy, ctx.cancellation);
        if (reply.has_value() && reply->size() <= 1)
        {
            reply = std::unexpected(Error{ErrorKind::kBadResponse, "security access reply is too short"});
        }
        if (!reply.has_value())
        {
            LogFailure(ctx, reply.error());
        }
        return reply;
    };
    LogInfo(ctx, "Requesting seed");
    return strict(bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed})
        .and_then(
            [&](const bytes::Bytes& reply) -> Result<bytes::Bytes>
            {
                const bytes::ByteView payload = uds::Payload(reply);
                if (payload.size() < 5 || payload[0] != uds::kSecurityAccessRequestSeed)
                {
                    LogError(ctx, "Wrong response from ECU: Not a valid answer");
                    return Fail(ErrorKind::kBadResponse, "invalid seed response");
                }
                LogInfo(ctx, "Seed request ok");
                return EcuFamilyTraits<Family>::SecurityKey(ctx, family, security, payload.subspan<1, 4>());
            })
        .and_then(
            [&](const bytes::Bytes& key)
            {
                LogInfo(ctx, "Sending seed key");
                return strict(bytes::ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, key));
            })
        .and_then(
            [&](const bytes::Bytes& reply) -> Status
            {
                if (uds::Payload(reply)[0] != uds::kSecurityAccessSendKey)
                {
                    const Error mismatch{ErrorKind::kBadResponse, "unexpected response during seed key"};
                    LogFailure(ctx, mismatch);
                    return std::unexpected(mismatch);
                }
                LogInfo(ctx, "Seed key ok");
                return {};
            });
}

// Logs the ECU's identity; a read prefixes the CAL ID to the image name.
Result<std::optional<std::string>> RequestIdentity(Context& context, bool read_operation,
                                                   std::optional<std::string>& rom_id)
{
    if (Status ecu_id = RequestEcuId(context, read_operation, rom_id); !ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
    }
    // These three fields share the 09/PID request and 49/PID/record envelope.
    // The order is fixed; only CVN renders binary bytes instead of text.
    constexpr std::array<std::pair<bytes::Byte, std::string_view>, 3> kFields{
        {{0x02, "VIN"}, {uds::kVehicleInfoPidCalId, "CAL ID"}, {0x06, "CVN"}}};
    std::optional<std::string> cal_id;
    for (const auto& [pid, label] : kFields)
    {
        LogInfo(context, std::format("Requesting {}", label));
        const auto reply = NonfatalQuery(context, bytes::Bytes{uds::kSidVehicleInfoRequest, pid});
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        if (!reply->has_value())
        {
            continue;
        }
        const auto& pdu = **reply;
        if (pdu.size() < 3 || pdu[0] != 0x49 || pdu[1] != pid)
        {
            LogWrongPdu(context, pdu);
            continue;
        }
        const std::string value = pid == 0x06 ? UppercaseHexCompact(bytes::ByteView(pdu).subspan(3))
                                              : std::string(pdu.begin() + 3, pdu.end());
        LogInfo(context, std::format("{}: {}", label, value));
        if (pid == uds::kVehicleInfoPidCalId)
        {
            cal_id = value;
            if (read_operation)
            {
                rom_id = value + "_" + rom_id.value_or(std::string{});
            }
        }
    }
    return cal_id;
}

template <typename Family> Status UploadKernel(Context& context, const KernelImage& kernel)
{
    using Traits = EcuFamilyTraits<Family>;
    if (const Status uploaded = context.kernel.TransferPayload(kernel, Traits::kTransferPolicy); !uploaded.has_value())
    {
        return uploaded;
    }
    if constexpr (std::same_as<Family, SubaruDensoSh7058CanDieselPlan>)
    {
        const std::size_t padded_size = ((kernel.bytes.size() + 127U) / 128U) * 128U;
        LogDebug(context, std::format("Data bytes sent: 0x{}", padded_size));
    }

    LogInfo(context, "Kernel uploaded, starting...");
    if (const Status exited =
            StrictService<Family>(context, bytes::Bytes{uds::kSidRequestTransferExit}, Traits::kTransferExitPolicy);
        !exited.has_value())
    {
        return exited;
    }
    if (const Status delay = context.clock.Sleep(kKernelStartDelay, context.cancellation); !delay.has_value())
    {
        return delay;
    }
    if (const Status started = StrictService<Family>(
            context, bytes::Bytes{uds::kSidRoutineControl, 0x01, 0x02, 0x02, 0x02}, kKernelStartPolicy);
        !started.has_value())
    {
        return started;
    }

    LogInfo(context, Traits::kRequestStartedKernelId);
    return StartedKernelId(context);
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
                    uds_client,
                    channel,
                    Traits::kProfile,
                    BeefCanKernel(transport, clock, cancellation, events, channel, Traits::kProfile)};

    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(context.events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    std::optional<std::string> rom_id;
    LogInfo(context, std::format("Connecting to {} CAN bootloader, please wait...", Traits::kUnit));
    LogInfo(context, "Checking if kernel is already running...");
    LogInfo(context, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = RunningKernelId(context);
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
        LogInfo(context, Traits::kInitializeConnection);
        const auto cal_id = RequestIdentity(context, read_operation, rom_id);
        if (!cal_id.has_value())
        {
            return std::unexpected(cal_id.error());
        }
        const auto security = Traits::PrepareSecurity(context, family, *cal_id);
        if (!security.has_value())
        {
            return std::unexpected(security.error());
        }
        auto sessions = RequestBootloaderSessions(context);
        if (!sessions.has_value())
        {
            return std::unexpected(sessions.error());
        }
        if (const Status unlocked = UnlockSecurity(context, family, *security); !unlocked.has_value())
        {
            return std::unexpected(unlocked.error());
        }
        if (const Status entered = EnterProgrammingSession(context, *sessions); !entered.has_value())
        {
            return std::unexpected(entered.error());
        }
        if (plan.Kernel().has_value())
        {
            context.events.Notice("Preparing, please wait...");
            LogInfo(context, std::format("Initializing {} CAN kernel upload, please wait...", Traits::kUnit));
            if (const Status uploaded = UploadKernel<Family>(context, *plan.Kernel()); !uploaded.has_value())
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

Result<FlashExecutionResult> RunDensoBeefCanEcu(const SubaruDensoSh7058CanPlan& family, const FlashPlan& plan,
                                                ICanFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events)
{
    return RunEcuExecution(family, plan, transport, clock, cancellation, events);
}

Result<FlashExecutionResult> RunDensoBeefCanEcu(const SubaruDensoSh7058CanDieselPlan& family, const FlashPlan& plan,
                                                ICanFlashTransport& transport, IClock& clock,
                                                const ICancellationToken& cancellation, IEventSink& events)
{
    return RunEcuExecution(family, plan, transport, clock, cancellation, events);
}

} // namespace fastecu::flash
