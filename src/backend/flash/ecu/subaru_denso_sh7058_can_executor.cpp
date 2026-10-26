#include "src/backend/flash/ecu/subaru_denso_sh7058_can_executor.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/can_flash_uds_channel.h"
#include "src/backend/flash/ecu/beef_can_session.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/subaru_denso_sh7058_can_plan.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
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
constexpr std::chrono::milliseconds kLongTimeout{800};
constexpr std::chrono::milliseconds kReadTimeout{2000};
constexpr std::chrono::milliseconds kExtraLongTimeout{3000};
constexpr std::chrono::milliseconds kKernelProbeDelay{200};
constexpr std::chrono::milliseconds kKernelStartDelay{100};
constexpr std::chrono::milliseconds kKernelStartReadTimeout{10};

constexpr std::size_t kKernelUploadBlockSize = 128;

constexpr bytes::Byte kKernelId = 0x01;

constexpr std::array<std::uint16_t, 16> kCobbSeedTable{
    0x9DDB, 0x9CFB, 0x9B9A, 0x6136, 0x59E1, 0xBA03, 0xD683, 0x7092,
    0x9E05, 0x8723, 0xF998, 0x15BB, 0xB8D5, 0xFF0C, 0x9D91, 0x24B9,
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
    SubaruDensoSh7058CanSecurity security;
    uds::UdsClient& uds;
    uds::IUdsChannel& channel;
    BeefCanSession session;
};

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

// The initial legacy probe continues when the frame is short, malformed, or
// addressed to another CAN id: those return no PDU. The strict post-upload
// probe fails on them.
Result<std::optional<bytes::Bytes>> ProbeKernelIdTolerantly(Context& context, bytes::ByteView request)
{
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(context, request, kLongTimeout, kKernelProbeDelay);
    if (!reply.has_value())
    {
        if (reply.error().kind == ErrorKind::kCancelled || reply.error().kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(reply.error());
        }
        LogError(context, "Wrong response from ECU while requesting kernel ID");
        return std::nullopt;
    }
    if (!reply->has_value())
    {
        return std::nullopt;
    }
    if (const Status checkpoint = CancelledIfRequested(context, "kernel ID probe cancelled"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    return reply;
}

// A timeout means no kernel is running.
Result<std::optional<bytes::Bytes>> ProbeKernelIdStrictly(Context& context, bytes::ByteView request)
{
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(context, request, kLongTimeout, kKernelProbeDelay);
    if (!reply.has_value() && reply.error().kind == ErrorKind::kTimeout)
    {
        return std::nullopt;
    }
    return reply;
}

Result<std::optional<std::string>> RequestKernelId(Context& context, bool tolerate_malformed)
{
    // request_kernel_id(), revision 59f4e442:1946-1980. The three trailing
    // zero bytes are outside the declared BEEF body but are present on the
    // legacy wire and therefore remain literal here.
    const bytes::Bytes request{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
    Result<std::optional<bytes::Bytes>> pdu =
        tolerate_malformed ? ProbeKernelIdTolerantly(context, request) : ProbeKernelIdStrictly(context, request);
    if (!pdu.has_value())
    {
        return std::unexpected(pdu.error());
    }
    if (!pdu->has_value())
    {
        return std::optional<std::string>{};
    }
    Result<BeefMessage> parsed = ParseBeef(**pdu);
    if (parsed.has_value() && parsed->opcode == static_cast<bytes::Byte>(kKernelId | 0x40U))
    {
        return std::optional<std::string>{std::string(parsed->payload.begin(), parsed->payload.end())};
    }
    if (tolerate_malformed)
    {
        LogError(context, "Wrong response from ECU while requesting kernel ID");
        return std::optional<std::string>{};
    }
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
}

Result<std::uint32_t> ReadRamLocation(Context& context, std::uint32_t address)
{
    // connect_bootloader(), revision 59f4e442:558-618. The vendor request
    // carries four consecutive 24-bit addresses rather than a range length.
    LogDebug(context, std::format("Reading RAM value at location: 0x{:x}", address));
    bytes::Bytes request{0xA8, 0x00};
    for (std::uint32_t offset = 0; offset < 4; ++offset)
    {
        const auto encoded = ComposeBe(bytes::U24{address + offset});
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

// Logs the ECU's identity and returns its CAL ID, which a read prefixes to
// the image name.
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
        return cal_id;
    }
    if (read_operation && cal_id->has_value())
    {
        rom_id = **cal_id + "_" + rom_id.value_or(std::string{});
    }
    if (Status cvn = context.session.RequestCvn(); !cvn.has_value())
    {
        return std::unexpected(cvn.error());
    }
    return cal_id;
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

Status UnlockSecurity(Context& context, const std::optional<RaceRomAltValues>& race_rom_alt)
{
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
    return {};
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
    Result<std::optional<std::string>> cal_id = RequestIdentity(context, read_operation, rom_id);
    if (!cal_id.has_value())
    {
        return std::unexpected(cal_id.error());
    }
    std::optional<RaceRomAltValues> race_rom_alt;
    if (context.security == SubaruDensoSh7058CanSecurity::kRaceRomAlt)
    {
        Result<RaceRomAltValues> values = ReadRaceRomAltValues(context, *cal_id);
        if (!values.has_value())
        {
            return std::unexpected(values.error());
        }
        race_rom_alt = *values;
    }

    return ConnectBeefProgrammingSession(
        context, [&race_rom_alt](Context& c) { return UnlockSecurity(c, race_rom_alt); }, kStrictPolicy);
}

Status UploadKernel(Context& context, const KernelImage& kernel)
{
    // upload_kernel(), revision 59f4e442:626-868. The unusual <= loop is
    // intentional: after every 128-byte payload block the oracle emits one
    // final address-only 0xB6 request.
    const Result<bytes::Bytes> encrypted = BeefKernelUploadPayload(kernel.bytes, kKernelUploadBlockSize, "petrol");
    if (!encrypted.has_value())
    {
        return std::unexpected(encrypted.error());
    }

    LogDebug(context, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    LogInfo(context, "Initialize kernel upload");
    if (const Status initialized = StrictPayload(context, BeefKernelDownloadRequest(kernel.load_address, *encrypted),
                                                 bytes::Bytes{0x20}, "kernel download");
        !initialized.has_value())
    {
        return initialized;
    }

    LogInfo(context, "Uploading kernel, please wait...");
    if (const Status uploaded = UploadBeefKernelBlocks(context, *encrypted, kernel.load_address, kKernelUploadBlockSize,
                                                       [&context](bytes::ByteView request)
                                                       { return DiscardB6Reply(context, request, kReadTimeout); });
        !uploaded.has_value())
    {
        return uploaded;
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

// The family's BEEF exchange as a callable for the shared read and write paths.
auto Exchanger(Context& context)
{
    return [&context](bytes::Byte opcode, bytes::ByteView payload, std::size_t minimum_payload,
                      std::chrono::milliseconds timeout)
    { return context.session.Exchange(opcode, payload, minimum_payload, timeout); };
}

} // namespace

Result<Iso15765Config> SubaruDensoSh7058CanExecutor::Setup(const SubaruDensoSh7058CanPlan& family) const
{
    return Iso15765ConfigFrom(family);
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

Result<FlashExecutionResult> SubaruDensoSh7058CanExecutor::Run(const SubaruDensoSh7058CanPlan& family,
                                                               const FlashPlan& plan, ICanFlashTransport& transport,
                                                               IClock& clock, const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    CanFlashUdsChannel channel(transport, family.request_id, family.response_id);
    uds::UdsClient uds_client(channel, clock, events);
    Context context{cancellation,
                    events,
                    clock,
                    transport,
                    family.security,
                    uds_client,
                    channel,
                    BeefCanSession(transport, clock, cancellation, events, channel, kSubaruDensoSh7058CanBeefProfile)};

    return RunBeefKernelOperation(
        plan, "Subaru Denso SH7058 petrol", context, ConnectBootloader, UploadKernel,
        [](Context& c, const FlashPlan& p, PhaseReporter& progress)
        { return ReadBeefMemory(c, p.TransferRegion(), progress, Exchanger(c)); },
        [](Context& c, const FlashPlan& p, PhaseSequence& phases, PhaseReporter& compare)
        { return c.session.WriteMemory(p, phases, compare); });
}

} // namespace fastecu::flash
