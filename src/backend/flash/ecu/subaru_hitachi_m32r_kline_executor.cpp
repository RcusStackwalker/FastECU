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
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

constexpr int kTimeoutMs = 2000;

bytes::Bytes Framed(bytes::ByteView payload, const SubaruHitachiM32rKlinePlan& p)
{
    return ssm_protocol::AddHeader(payload, p.tester_id, p.target_id);
}

Result<std::optional<bytes::Bytes>> ExchangeOptional(IKlineFlashTransport& transport,
                                                     const ICancellationToken& cancellation, bytes::ByteView payload,
                                                     const SubaruHitachiM32rKlinePlan& p, int timeout = kTimeoutMs)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before write");
    }
    const bytes::Bytes request = Framed(payload, p);
    auto written = transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line write");
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after write");
    }
    auto response = transport.Read(std::chrono::milliseconds{timeout}, cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after read");
    }
    return std::move(*response);
}

Result<bytes::Bytes> Exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruHitachiM32rKlinePlan& p)
{
    auto response = ExchangeOptional(transport, cancellation, payload, p);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no response from ECU");
    }
    return std::move(**response);
}

Status ExpectPrefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    if (response.size() < 4 + prefix.size())
    {
        return Fail(ErrorKind::kBadResponse, "response is too short");
    }
    std::size_t i = 4;
    for (bytes::Byte value : prefix)
    {
        if (response[i++] != value)
        {
            return Fail(ErrorKind::kBadResponse, "wrong response from ECU");
        }
    }
    return {};
}

Result<std::string> ParseRomId(bytes::ByteView response)
{
    if (auto valid = ExpectPrefix(response, {0xff}); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (response.size() < 13)
    {
        return Fail(ErrorKind::kBadResponse, "ECU ID response is too short");
    }
    std::string id;
    for (std::size_t i = 8; i < 13; ++i)
    {
        id += std::format("{:02X}", response[i]);
    }
    return id + '_';
}

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex = {0x3275, 0x6ad8, 0x1062, 0x512b, 0xd695, 0x7640,
                                                             0x25f6, 0xac45, 0x6803, 0xe5da, 0xc821, 0x36bf,
                                                             0xa433, 0x3f41, 0x842c, 0x05d9};
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kIndexTransformationStock);
}

bytes::Bytes Encrypt(bytes::ByteView image)
{
    static constexpr std::array<std::uint16_t, 4> kIndex = {0x78f1, 0x2962, 0x9312, 0x7c03};
    return ssm_protocol::CalculatePayload(image, static_cast<std::uint32_t>(image.size()), kIndex,
                                          ssm_protocol::kIndexTransformationStock);
}

Status RequestPrefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation, bytes::Bytes request,
                     std::initializer_list<bytes::Byte> expected, const SubaruHitachiM32rKlinePlan& p)
{
    auto response = Exchange(transport, cancellation, request, p);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    return ExpectPrefix(*response, expected);
}

Status AuthenticatedSession(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                            const SubaruHitachiM32rKlinePlan& p, bool include_bf)
{
    if (include_bf)
    {
        auto id = Exchange(transport, cancellation, bytes::Bytes{0xbf}, p);
        if (!id.has_value())
        {
            return std::unexpected(id.error());
        }
        if (auto valid = ParseRomId(*id); !valid.has_value())
        {
            return std::unexpected(valid.error());
        }
        if (auto s = RequestPrefix(transport, cancellation, {0x81}, {0xc1}, p); !s.has_value())
        {
            return s;
        }
    }
    if (auto s = RequestPrefix(transport, cancellation, {0x83, 0}, {0xc3}, p); !s.has_value())
    {
        return s;
    }
    auto seed =
        Exchange(transport, cancellation, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p);
    if (!seed.has_value())
    {
        return std::unexpected(seed.error());
    }
    if (auto s = ExpectPrefix(*seed, {0x67, uds::kSecurityAccessRequestSeed}); !s.has_value())
    {
        return s;
    }
    if (seed->size() < 10)
    {
        return Fail(ErrorKind::kBadResponse, "seed response is too short");
    }
    bytes::Bytes key_request =
        ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, SeedKey(bytes::ByteView{*seed}.subspan(6, 4)));
    if (Status key_status = p.session_mode == HitachiM32rKlineSessionMode::kRecovery
                                ? RequestPrefix(transport, cancellation, std::move(key_request), {0x67}, p)
                                : RequestPrefix(transport, cancellation, std::move(key_request),
                                                {0x67, uds::kSecurityAccessSendKey}, p);
        !key_status.has_value())
    {
        return key_status;
    }
    return RequestPrefix(transport, cancellation, {uds::kSidDiagnosticSessionControl, 0x85, 0x02}, {0x50}, p);
}

Result<std::string> PrepareRead(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                const SubaruHitachiM32rKlinePlan& p)
{
    if (auto baud = transport.SetBaud(p.read_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    auto probe = ExchangeOptional(transport, cancellation, bytes::Bytes{0xbf}, p);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (*probe)
    {
        if (auto id = ParseRomId(**probe); id)
        {
            return id;
        }
    }
    if (auto baud = transport.SetBaud(p.initial_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    auto initial = Exchange(transport, cancellation, bytes::Bytes{0xbf}, p);
    if (!initial.has_value())
    {
        return std::unexpected(initial.error());
    }
    auto id = ParseRomId(*initial);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (auto s = RequestPrefix(transport, cancellation, {0xb8, 0x00, 0x00, 0x00, 0x75}, {0xf8}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (auto baud = transport.SetBaud(p.read_baud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    if (auto s = RequestPrefix(transport, cancellation, {0xbf}, {0xff}, p); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    return id;
}

Status PrepareWrite(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                    const SubaruHitachiM32rKlinePlan& p)
{
    if (p.session_mode == HitachiM32rKlineSessionMode::kRecovery)
    {
        if (auto baud = transport.SetBaud(p.initial_baud); !baud.has_value())
        {
            return baud;
        }
        bool awake = false;
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            auto response = ExchangeOptional(transport, cancellation, bytes::Bytes{0x81}, p, 50);
            if (!response.has_value())
            {
                return std::unexpected(response.error());
            }
            if (!response->has_value())
            {
                continue;
            }
            if (auto valid = ExpectPrefix(**response, {0xc1}); !valid.has_value())
            {
                return valid;
            }
            awake = true;
            break;
        }
        if (!awake)
        {
            return Fail(ErrorKind::kTimeout, "recovery wake sequence exhausted 1000 attempts");
        }
        return AuthenticatedSession(transport, cancellation, p, false);
    }
    if (auto baud = transport.SetBaud(p.write_baud); !baud.has_value())
    {
        return baud;
    }
    auto probe =
        ExchangeOptional(transport, cancellation, bytes::Bytes{uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0}, p);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (*probe && (*probe)->size() >= 6 && (**probe)[4] == 0x74 && (**probe)[5] == 0x84)
    {
        return {};
    }
    if (auto baud = transport.SetBaud(p.initial_baud); !baud.has_value())
    {
        return baud;
    }
    return AuthenticatedSession(transport, cancellation, p, true);
}

Result<bytes::Bytes> ReadRom(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                             IEventSink& events, const SubaruHitachiM32rKlinePlan& p)
{
    bytes::Bytes rom;
    rom.reserve(0x80000);
    for (std::uint32_t logical = 0; logical < 0x80000; logical += p.chunk_size)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = logical + p.read_address_bias;
        auto response = Exchange(transport, cancellation,
                                 ComposeBe(0xa0_b, 0x00_b, 0x00_b, U24(address), bytes::Byte(p.chunk_size - 1)), p);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->size() != p.chunk_size + 6 || (*response)[4] != 0xe0)
        {
            return Fail(ErrorKind::kBadResponse, "ROM read response must contain exactly 128 data bytes");
        }
        rom.insert(rom.end(), response->begin() + 5, response->end() - 1);
        events.Progress(static_cast<int>(logical + p.chunk_size), 0x80000);
    }
    return rom;
}

Status EraseRom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                const SubaruHitachiM32rKlinePlan& p)
{
    const bytes::Bytes request =
        Framed(bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x0f, 0xff, 0xff, 0xff}, p);
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before erase");
    }
    auto written = transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line erase write");
    }

    bytes::Bytes response;
    int attempts_remaining = 20;
    while (response.size() <= 5 && attempts_remaining > 0)
    {
        --attempts_remaining;
        auto fragment = transport.Read(500ms, cancellation);
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
            if (auto slept = clock.Sleep(500ms, cancellation); !slept.has_value())
            {
                return slept;
            }
        }
    }
    return ExpectPrefix(response, {0x71, uds::kRoutineControlStop});
}

Status WriteRom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                IEventSink& events, const SubaruHitachiM32rKlinePlan& p, const FlashPlan& plan)
{
    if (auto baud = transport.SetBaud(p.write_baud); !baud.has_value())
    {
        return baud;
    }
    if (auto s =
            RequestPrefix(transport, cancellation, {uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0}, {0x74}, p);
        !s.has_value())
    {
        return s;
    }
    if (auto slept = clock.Sleep(200ms, cancellation); !slept.has_value())
    {
        return slept;
    }
    if (auto erased = EraseRom(transport, clock, cancellation, p); !erased.has_value())
    {
        return erased;
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled after erase");
    }
    const bytes::Bytes encrypted = Encrypt(plan.ImageOrEmpty());
    for (std::uint32_t address = 0; address < 0x80000; address += p.chunk_size)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM write");
        }
        const bytes::Bytes request =
            ComposeBe(uds::kSidTransferData, U24(address), bytes::ByteView(encrypted).subspan(address, p.chunk_size));
        auto ack = ExchangeOptional(transport, cancellation, request, p);
        if (!ack.has_value())
        {
            return std::unexpected(ack.error());
        }
        if (*ack && (*ack)->size() > 4 && (**ack)[4] != 0x76)
        {
            return Fail(ErrorKind::kBadResponse, "write data failed");
        }
        events.Progress(static_cast<int>(address + p.chunk_size), 0x80000);
    }
    if (auto slept = clock.Sleep(300ms, cancellation); !slept.has_value())
    {
        return slept;
    }
    auto checksum = ExchangeOptional(transport, cancellation,
                                     bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02}, p);
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    if (!checksum->has_value() || (*checksum)->empty())
    {
        return Fail(ErrorKind::kTimeout, "no final checksum response");
    }
    return {};
}
} // namespace

Result<KlineConfig> SubaruHitachiM32rKlineExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruHitachiM32rKlinePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& p = std::get<SubaruHitachiM32rKlinePlan>(plan.FamilyPlan());
    return NonIso14230KlineConfigFrom(p);
}

Result<FlashExecutionResult> SubaruHitachiM32rKlineExecutor::Execute(const FlashPlan& plan,
                                                                     IKlineFlashTransport& transport, IClock& clock,
                                                                     const ICancellationToken& cancellation,
                                                                     IEventSink& events)
{
    if (const Status match = CheckFamily(plan, FlashFamily::kSubaruHitachiM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (const Status valid = ValidateSubaruHitachiM32rKlinePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, "cancelled before setup");
    }
    const auto& p = std::get<SubaruHitachiM32rKlinePlan>(plan.FamilyPlan());
    Result<FlashExecutionResult> outcome = Fail(ErrorKind::kInternal, "unreachable");
    if (auto header = transport.SetAddIso14230Header(false); !header.has_value())
    {
        outcome = std::unexpected(header.error());
    }
    else if (plan.Operation() == FlashOperation::kRead)
    {
        auto id = PrepareRead(transport, cancellation, p);
        if (!id.has_value())
        {
            outcome = std::unexpected(id.error());
        }
        else if (auto rom = ReadRom(transport, cancellation, events, p); !rom.has_value())
        {
            outcome = std::unexpected(rom.error());
        }
        else
        {
            outcome = FlashExecutionResult{FlashOperation::kRead, std::move(*rom), std::move(*id)};
        }
    }
    else if (auto prepared = PrepareWrite(transport, cancellation, p); !prepared.has_value())
    {
        outcome = std::unexpected(prepared.error());
    }
    else if (auto written = WriteRom(transport, clock, cancellation, events, p, plan); !written.has_value())
    {
        outcome = std::unexpected(written.error());
    }
    else
    {
        outcome = FlashExecutionResult{FlashOperation::kWrite, std::nullopt, std::nullopt};
    }
    if (!outcome.has_value())
    {
        return std::unexpected(outcome.error());
    }
    return outcome;
}
} // namespace fastecu::flash
