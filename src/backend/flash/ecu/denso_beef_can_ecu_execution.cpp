#include "src/backend/flash/ecu/denso_beef_can_ecu_execution.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
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
// Petrol's matching oracle is flash_ecu_subaru_denso_sh7058_can_operation.cpp:
// connect_bootloader (112-621), upload_kernel (626-870), and security
// (1765-1906). Its five security variants and RaceRom-alt RAM preparation
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
    ICanFlashTransport& transport;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
    BeefCanSession session;
    BeefCanKernel kernel;
};

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
            context.session.LogFailure(reply.error());
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
    // connect_bootloader(), revision 59f4e442:558-618. The vendor request
    // carries four consecutive 24-bit addresses rather than a range length.
    LogDebug(context, std::format("Reading RAM value at location: 0x{:x}", address));
    bytes::Bytes request{0xA8, 0x00};
    for (std::uint32_t offset = 0; offset < 4; ++offset)
    {
        const auto encoded = bytes::ComposeBe(bytes::U24{address + offset});
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
    Result<std::uint32_t> seed_alter = ReadRamLocation(context, addresses->seed_address);
    if (!seed_alter.has_value())
    {
        return std::unexpected(seed_alter.error());
    }
    LogDebug(context, std::format("Value at RAM loc 0x{:x} is: 0x{:x}", addresses->seed_address, *seed_alter));
    Result<std::uint32_t> xor_value = ReadRamLocation(context, addresses->xor_address);
    if (!xor_value.has_value())
    {
        return std::unexpected(xor_value.error());
    }
    LogDebug(context, std::format("Value at RAM loc 0x{:x} is: 0x{:x}", addresses->xor_address, *xor_value));
    const RaceRomAltValues values{.seed_alter = *seed_alter,
                                  .xor_byte_1 = static_cast<bytes::Byte>((*xor_value >> 8U) & 0xFFU),
                                  .xor_byte_2 = static_cast<bytes::Byte>(*xor_value & 0xFFU)};
    LogInfo(context, std::format("Seed alter is: 0x{:x}", values.seed_alter));
    LogInfo(context, std::format("XOR values are: 0x{:x} and 0x{:x}", values.xor_byte_1, values.xor_byte_2));
    return values;
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
                                            const SecurityState& alt, bytes::ByteView seed)
    {
        if (seed.size() != 4)
        {
            return Fail(ErrorKind::kBadResponse, "petrol security seed must contain four bytes");
        }
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
                                            bytes::ByteView seed)
    {
        if (seed.size() != 4)
        {
            return Fail(ErrorKind::kBadResponse, "diesel security seed must contain four bytes");
        }
        return DensoSeedKey(seed);
    }
};

// Logs the ECU's identity; a read prefixes the CAL ID to the image name.
Result<std::optional<std::string>> RequestIdentity(Context& context, bool read_operation,
                                                   std::optional<std::string>& rom_id)
{
    if (Status ecu_id = context.session.RequestEcuId(read_operation, rom_id); !ecu_id.has_value())
    {
        return std::unexpected(ecu_id.error());
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
    if (const Status cvn = context.session.RequestCvn(); !cvn.has_value())
    {
        return std::unexpected(cvn.error());
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
        auto sessions = context.session.RequestBootloaderSessions();
        if (!sessions.has_value())
        {
            return std::unexpected(sessions.error());
        }
        const auto seed_key = [&](Context& c, bytes::ByteView seed)
        { return Traits::SecurityKey(c, family, *security, seed); };
        if (const Status unlocked = UnlockBeefSecurity(context, seed_key, kStrictPolicy); !unlocked.has_value())
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
