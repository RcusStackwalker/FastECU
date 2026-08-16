#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"

#include <array>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

constexpr int kTimeoutMs = 2000;

bytes::Bytes framed(bytes::ByteView payload, const SubaruHitachiM32rKlinePlan& p)
{
    return SsmProtocol::addHeader(payload, p.tester_id, p.target_id);
}

Result<std::optional<bytes::Bytes>> exchange_optional(IKlineFlashTransport& transport,
                                                      const ICancellationToken& cancellation, bytes::ByteView payload,
                                                      const SubaruHitachiM32rKlinePlan& p, int timeout = kTimeoutMs)
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
    auto response = transport.read(timeout, cancellation);
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
                              bytes::ByteView payload, const SubaruHitachiM32rKlinePlan& p)
{
    auto response = exchange_optional(transport, cancellation, payload, p);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, "no response from ECU");
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
            return fail(ErrorKind::BadResponse, "wrong response from ECU");
        }
    }
    return {};
}

Result<std::string> parse_rom_id(bytes::ByteView response)
{
    if (auto valid = expect_prefix(response, {0xff}); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (response.size() < 13)
    {
        return fail(ErrorKind::BadResponse, "ECU ID response is too short");
    }
    std::string id;
    for (std::size_t i = 8; i < 13; ++i)
    {
        id += std::format("{:02X}", response[i]);
    }
    return id + '_';
}

bytes::Bytes seed_key(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> index = {0x3275, 0x6ad8, 0x1062, 0x512b, 0xd695, 0x7640,
                                                            0x25f6, 0xac45, 0x6803, 0xe5da, 0xc821, 0x36bf,
                                                            0xa433, 0x3f41, 0x842c, 0x05d9};
    static constexpr std::array<std::uint8_t, 32> transform = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8, 0xa, 0xd, 0x2,
                                                               0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2,
                                                               0xd, 0x9, 0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculateSeedKey(seed, index, transform);
}

bytes::Bytes encrypt(bytes::ByteView image)
{
    static constexpr std::array<std::uint16_t, 4> index = {0x78f1, 0x2962, 0x9312, 0x7c03};
    static constexpr std::array<std::uint8_t, 32> transform = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8, 0xa, 0xd, 0x2,
                                                               0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2,
                                                               0xd, 0x9, 0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()), index, transform);
}

Status request_prefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation, bytes::Bytes request,
                      std::initializer_list<bytes::Byte> expected, const SubaruHitachiM32rKlinePlan& p)
{
    auto response = exchange(transport, cancellation, request, p);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return expect_prefix(*response, expected);
}

Status authenticated_session(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                             const SubaruHitachiM32rKlinePlan& p, bool include_bf)
{
    if (include_bf)
    {
        auto id = exchange(transport, cancellation, bytes::Bytes{0xbf}, p);
        if (!id.has_value())
        {
            return std::unexpected(id.error());
        }
        if (auto valid = parse_rom_id(*id); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        if (auto s = request_prefix(transport, cancellation, {0x81}, {0xc1}, p); !s.has_value())
        {
            return s;
        }
    }
    if (auto s = request_prefix(transport, cancellation, {0x83, 0}, {0xc3}, p); !s.has_value())
    {
        return s;
    }
    auto seed =
        exchange(transport, cancellation, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p);
    if (!seed.has_value())
    {
        return std::unexpected(seed.error());
    }
    if (auto s = expect_prefix(*seed, {0x67, uds::kSecurityAccessRequestSeed}); !s.has_value())
    {
        return s;
    }
    if (seed->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }
    bytes::Bytes key_request =
        composeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, seed_key(bytes::ByteView{*seed}.subspan(6, 4)));
    if (Status key_status = p.session_mode == HitachiM32rKlineSessionMode::Recovery
                                ? request_prefix(transport, cancellation, std::move(key_request), {0x67}, p)
                                : request_prefix(transport, cancellation, std::move(key_request),
                                                 {0x67, uds::kSecurityAccessSendKey}, p);
        !key_status.has_value())
    {
        return key_status;
    }
    return request_prefix(transport, cancellation, {uds::kSidDiagnosticSessionControl, 0x85, 0x02}, {0x50}, p);
}

Result<std::string> prepare_read(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                 const SubaruHitachiM32rKlinePlan& p)
{
    if (auto baud = transport.setBaud(p.read_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    auto probe = exchange_optional(transport, cancellation, bytes::Bytes{0xbf}, p);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (*probe)
    {
        if (auto id = parse_rom_id(**probe); id)
        {
            return id;
        }
    }
    if (auto baud = transport.setBaud(p.initial_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    auto initial = exchange(transport, cancellation, bytes::Bytes{0xbf}, p);
    if (!initial.has_value())
    {
        return std::unexpected(initial.error());
    }
    auto id = parse_rom_id(*initial);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0xb8, 0x00, 0x00, 0x00, 0x75}, {0xf8}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (auto baud = transport.setBaud(p.read_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (auto s = request_prefix(transport, cancellation, {0xbf}, {0xff}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    return id;
}

Status prepare_write(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                     const SubaruHitachiM32rKlinePlan& p)
{
    if (p.session_mode == HitachiM32rKlineSessionMode::Recovery)
    {
        if (auto baud = transport.setBaud(p.initial_baud); !baud.has_value())
        {
            return baud;
        }
        bool awake = false;
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            auto response = exchange_optional(transport, cancellation, bytes::Bytes{0x81}, p, 50);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->has_value())
            {
                continue;
            }
            if (auto valid = expect_prefix(**response, {0xc1}); !valid.has_value())
            {
                return valid;
            }
            awake = true;
            break;
        }
        if (!awake)
        {
            return fail(ErrorKind::Timeout, "recovery wake sequence exhausted 1000 attempts");
        }
        return authenticated_session(transport, cancellation, p, false);
    }
    if (auto baud = transport.setBaud(p.write_baud); !baud.has_value())
    {
        return baud;
    }
    auto probe = exchange_optional(transport, cancellation,
                                   bytes::Bytes{uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0}, p);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (*probe && (*probe)->size() >= 6 && (**probe)[4] == 0x74 && (**probe)[5] == 0x84)
    {
        return {};
    }
    if (auto baud = transport.setBaud(p.initial_baud); !baud.has_value())
    {
        return baud;
    }
    return authenticated_session(transport, cancellation, p, true);
}

Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruHitachiM32rKlinePlan& p)
{
    bytes::Bytes rom;
    rom.reserve(0x80000);
    for (std::uint32_t logical = 0; logical < 0x80000; logical += p.chunk_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = logical + p.read_address_bias;
        auto response = exchange(transport, cancellation,
                                 composeBe(0xa0_b, 0x00_b, 0x00_b, u24(address), bytes::Byte(p.chunk_size - 1)), p);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->size() != p.chunk_size + 6 || (*response)[4] != 0xe0)
        {
            return fail(ErrorKind::BadResponse, "ROM read response must contain exactly 128 data bytes");
        }
        rom.insert(rom.end(), response->begin() + 5, response->end() - 1);
        events.progress(static_cast<int>(logical + p.chunk_size), 0x80000);
    }
    return rom;
}

Status erase_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 const SubaruHitachiM32rKlinePlan& p)
{
    const bytes::Bytes request =
        framed(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x0f, 0xff, 0xff, 0xff}, p);
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before erase");
    }
    auto written = transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line erase write");
    }

    bytes::Bytes response;
    int attempts_remaining = 20;
    while (response.size() <= 5 && attempts_remaining > 0)
    {
        --attempts_remaining;
        auto fragment = transport.read(500, cancellation);
        if (!fragment.has_value())
        {
            return std::unexpected(fragment.error());
        }
        if (fragment->has_value())
        {
            response.insert(response.end(), (*fragment)->begin(), (*fragment)->end());
        }
        if (response.size() <= 5)
        {
            if (auto slept = clock.sleep(500, cancellation); !slept.has_value())
            {
                return slept;
            }
        }
    }
    return expect_prefix(response, {0x71, uds::kRoutineControlStop});
}

Status write_rom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 IEventSink& events, const SubaruHitachiM32rKlinePlan& p, const FlashPlan& plan)
{
    if (auto baud = transport.setBaud(p.write_baud); !baud.has_value())
    {
        return baud;
    }
    if (auto s =
            request_prefix(transport, cancellation, {uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0}, {0x74}, p);
        !s.has_value())
    {
        return s;
    }
    if (auto slept = clock.sleep(200, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (auto erased = erase_rom(transport, clock, cancellation, p); !erased.has_value())
    {
        return erased;
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after erase");
    }
    const bytes::Bytes encrypted = encrypt(*plan.image());
    for (std::uint32_t address = 0; address < 0x80000; address += p.chunk_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM write");
        }
        const bytes::Bytes request =
            composeBe(uds::kSidTransferData, u24(address), bytes::ByteView(encrypted).subspan(address, p.chunk_size));
        auto ack = exchange_optional(transport, cancellation, request, p);
        if (!ack.has_value())
        {
            return std::unexpected(ack.error());
        }
        if (*ack && (*ack)->size() > 4 && (**ack)[4] != 0x76)
        {
            return fail(ErrorKind::BadResponse, "write data failed");
        }
        events.progress(static_cast<int>(address + p.chunk_size), 0x80000);
    }
    if (auto slept = clock.sleep(300, cancellation); !slept.has_value())
    {
        return slept;
    }
    auto checksum = exchange_optional(transport, cancellation,
                                      bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02}, p);
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value() || (*checksum)->empty())
    {
        return fail(ErrorKind::Timeout, "no final checksum response");
    }
    return {};
}
} // namespace

Result<FlashExecutionResult> SubaruHitachiM32rKlineExecutor::execute(const FlashPlan& plan, IFlashTransport& transport,
                                                                     IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (auto match = check_family_transport_match(plan, FlashFamily::SubaruHitachiM32rKline, TransportKind::Kline);
        !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (auto valid = validate_subaru_hitachi_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    auto *kline = dynamic_cast<IKlineFlashTransport *>(&transport);
    if (kline == nullptr)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement IKlineFlashTransport");
    }
    const auto& p = std::get<SubaruHitachiM32rKlinePlan>(plan.family_plan());
    if (auto configured = kline->configure({p.initial_baud, false, p.tester_id, p.target_id}); !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (auto opened = kline->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    Result<FlashExecutionResult> outcome = fail(ErrorKind::Internal, "unreachable");
    if (auto header = kline->set_add_iso14230_header(false); !header.has_value())
    {
        outcome = std::unexpected(header.error());
    }
    else if (plan.operation() == FlashOperation::Read)
    {
        auto id = prepare_read(*kline, cancellation, p);
        if (!id.has_value())
        {
            outcome = std::unexpected(id.error());
        }
        else if (auto rom = read_rom(*kline, cancellation, events, p); !rom.has_value())
        {
            outcome = std::unexpected(rom.error());
        }
        else
        {
            outcome = FlashExecutionResult{FlashOperation::Read, std::move(*rom), std::move(*id)};
        }
    }
    else if (auto prepared = prepare_write(*kline, cancellation, p); !prepared.has_value())
    {
        outcome = std::unexpected(prepared.error());
    }
    else if (auto written = write_rom(*kline, clock, cancellation, events, p, plan); !written.has_value())
    {
        outcome = std::unexpected(written.error());
    }
    else
    {
        outcome = FlashExecutionResult{FlashOperation::Write, std::nullopt, std::nullopt};
    }
    Status closed = kline->close();
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    if (!closed.has_value())
    {
        return std::unexpected(closed.error());
    }
    return outcome;
}
} // namespace fastecu::flash
