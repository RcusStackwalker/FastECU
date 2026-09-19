#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_executor.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/subaru_tcu_hitachi_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using namespace bytes::literals;
using namespace std::chrono_literals;

// The following four constants back the read_rom loop below; only
// kConnectTimeoutMs is used by connect_bootloader().
constexpr std::uint32_t kRomSize = 0x80000;
// Legacy serial_read_timeout, used by every connect_bootloader() exchange.
constexpr int kConnectTimeoutMs = 2000;
// Legacy receive_timeout, used by send_sid_a0_block_read().
constexpr int kBlockTimeoutMs = 500;
// The delay(100) on each side of send_sid_a0_block_read()'s read.
constexpr auto kBlockDelay = 100ms;
// read_a0_rom retries a block up to five times before giving up.
constexpr int kBlockAttempts = 5;

bytes::Bytes framed(bytes::ByteView payload, const SubaruTcuHitachiM32rKlinePlan& p)
{
    return SsmProtocol::addHeader(payload, p.tester_id, p.target_id);
}

Result<std::optional<bytes::Bytes>> exchange_optional(IKlineFlashTransport& transport,
                                                      const ICancellationToken& cancellation, bytes::ByteView payload,
                                                      const SubaruTcuHitachiM32rKlinePlan& p, int timeout)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    const bytes::Bytes request = framed(payload, p);
    auto written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    auto response = transport.read(std::chrono::milliseconds{timeout}, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    return std::move(*response);
}

Result<bytes::Bytes> exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruTcuHitachiM32rKlinePlan& p, int timeout)
{
    auto response = exchange_optional(transport, cancellation, payload, p, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from TCU");
    }
    return std::move(**response);
}

// send_sid_a0_block_read's exact wire pacing: write, delay(100), read,
// delay(100) -- the 100ms gap sits BETWEEN the write and the read, unlike the
// connect-path exchange()/exchange_optional() helpers, which write and read
// back to back with no pacing delay of their own. This family's block-read
// wire timing has no hardware qualification behind it, so this reproduces the
// legacy order exactly rather than "improving" it. Keeps the same three
// cancellation checkpoints as exchange_optional (before write, after write,
// after read); clock.sleep() itself also observes cancellation.
Result<bytes::Bytes> exchange_block_read(IKlineFlashTransport& transport, IClock& clock,
                                         const ICancellationToken& cancellation, bytes::ByteView payload,
                                         const SubaruTcuHitachiM32rKlinePlan& p, int timeout)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    const bytes::Bytes request = framed(payload, p);
    auto written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    if (auto slept = clock.sleep(kBlockDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    auto response = transport.read(std::chrono::milliseconds{timeout}, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after read");
    }
    if (auto slept = clock.sleep(kBlockDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from TCU");
    }
    return std::move(**response);
}

Status expect_prefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    if (response.size() < 4 + prefix.size())
    {
        return fail(ErrorKind::BadResponse, "response is too short");
    }
    std::size_t i = 4;
    for (bytes::Byte value : prefix)
    {
        if (response[i++] != value)
        {
            return fail(ErrorKind::BadResponse, "wrong response from TCU");
        }
    }
    return {};
}

Status request_prefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation, bytes::Bytes request,
                      std::initializer_list<bytes::Byte> expected, const SubaruTcuHitachiM32rKlinePlan& p)
{
    auto response = exchange(transport, cancellation, request, p, kConnectTimeoutMs);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return expect_prefix(*response, expected);
}

// Legacy: received.remove(0, 8); received.remove(5, ...) -- bytes 8..12 of the
// 0xBF response, hex-encoded, with a trailing underscore.
Result<std::string> parse_rom_id(bytes::ByteView response)
{
    if (auto valid = expect_prefix(response, {0xff}); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (response.size() < 13)
    {
        return fail(ErrorKind::BadResponse, "TCU ID response is too short");
    }
    std::string id;
    for (std::size_t i = 8; i < 13; ++i)
    {
        id += std::format("{:02X}", response[i]);
    }
    return id + '_';
}

// This family's own 16-entry generation table. Its index transformation was
// compared byte for byte against SsmProtocol::kIndexTransformationStock and is
// identical, so only the generation table is family-specific.
bytes::Bytes seed_key(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> index = {0x0FE9, 0xCA58, 0x5E90, 0xDFF1, 0x690B, 0xF591,
                                                            0x1794, 0x5C7B, 0xA7BF, 0x98E5, 0x0B63, 0xA1C9,
                                                            0x79BF, 0xF413, 0x82B1, 0xA895};
    return SsmProtocol::calculateSeedKey(seed, index, SsmProtocol::kIndexTransformationStock);
}

Result<std::string> connect_bootloader(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                       const SubaruTcuHitachiM32rKlinePlan& p)
{
    auto id_response = exchange(transport, cancellation, bytes::Bytes{0xbf}, p, kConnectTimeoutMs);
    if (!id_response.has_value())
    {
        return std::unexpected(id_response.error());
    }
    auto id = parse_rom_id(*id_response);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0x81}, {0xc1}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0x83, 0x00}, {0xc3}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    auto seed = exchange(transport, cancellation,
                         bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p, kConnectTimeoutMs);
    if (!seed.has_value())
    {
        return std::unexpected(seed.error());
    }
    if (auto s = expect_prefix(*seed, {0x67, uds::kSecurityAccessRequestSeed}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (seed->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }
    bytes::Bytes key_request =
        composeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, seed_key(bytes::ByteView{*seed}.subspan(6, 4)));
    if (auto s =
            request_prefix(transport, cancellation, std::move(key_request), {0x67, uds::kSecurityAccessSendKey}, p);
        !s.has_value())
    {
        return std::unexpected(s.error());
    }
    return id;
}

Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruTcuHitachiM32rKlinePlan& p);
} // namespace

Result<KlineConfig> SubaruTcuHitachiM32rKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (const Status match = check_family(plan, FlashFamily::SubaruTcuHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& p = std::get<SubaruTcuHitachiM32rKlinePlan>(plan.family_plan());
    // Unlike the ECU sibling, this family sets is_iso14230_connection(true),
    // so non_iso14230_kline_config_from() must not be used here.
    return KlineConfig{
        .baud = p.baud,
        .iso14230 = true,
        .tester_id = p.tester_id,
        .target_id = p.target_id,
    };
}

Result<FlashExecutionResult> SubaruTcuHitachiM32rKlineExecutor::execute(const FlashPlan& plan,
                                                                        IKlineFlashTransport& transport, IClock& clock,
                                                                        const ICancellationToken& cancellation,
                                                                        IEventSink& events)
{
    if (const Status match = check_family(plan, FlashFamily::SubaruTcuHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = validate_subaru_tcu_hitachi_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (plan.operation() != FlashOperation::Read)
    {
        return fail(ErrorKind::Unsupported, "Subaru TCU Hitachi M32R K-Line supports read only");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    const auto& p = std::get<SubaruTcuHitachiM32rKlinePlan>(plan.family_plan());
    if (auto header = transport.set_add_iso14230_header(false); !header.has_value())
    {
        return std::unexpected(header.error());
    }
    auto id = connect_bootloader(transport, cancellation, p);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    auto rom = read_rom(transport, clock, cancellation, events, p);
    if (!rom.has_value())
    {
        return std::unexpected(rom.error());
    }
    return FlashExecutionResult{FlashOperation::Read, std::move(*rom), std::move(*id)};
}

namespace
{
// read_a0_rom's block-read loop: 96-byte blocks (the final block is whatever
// remains, 32 bytes for the 0x80000 ROM), up to five attempts per block while
// the response is too short to carry data (legacy: received.length() <= 5).
Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruTcuHitachiM32rKlinePlan& p)
{
    bytes::Bytes rom;
    rom.reserve(kRomSize);
    for (std::uint32_t address = 0; address < kRomSize; address += p.block_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM read");
        }
        const std::uint32_t length = std::min(p.block_size, kRomSize - address);
        const bytes::Bytes request =
            composeBe(0xa0_b, 0x00_b, bytes::u24(address), static_cast<bytes::Byte>(length - 1));

        // read_a0_rom's retry loop: up to five attempts while the response is
        // too short to carry data. Each attempt keeps the legacy
        // write/delay(100)/read/delay(100) pacing via exchange_block_read.
        std::optional<bytes::Bytes> block;
        for (int attempt = 0; attempt < kBlockAttempts; ++attempt)
        {
            auto response = exchange_block_read(transport, clock, cancellation, request, p, kBlockTimeoutMs);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (response->size() > 5)
            {
                block = std::move(*response);
                break;
            }
        }
        if (!block.has_value())
        {
            return fail(ErrorKind::BadResponse,
                        std::format("no block-read response at 0x{:06x} after {} attempts", address, kBlockAttempts));
        }
        // 5 header bytes, `length` data bytes, 1 checksum byte.
        if (block->size() != length + 6)
        {
            return fail(ErrorKind::BadResponse, std::format("block read at 0x{:06x} returned {} bytes, expected {}",
                                                            address, block->size(), length + 6));
        }
        rom.insert(rom.end(), block->begin() + 5, block->end() - 1);
        events.progress(static_cast<int>(address + length), static_cast<int>(kRomSize));
    }
    return rom;
}
} // namespace
} // namespace fastecu::flash
