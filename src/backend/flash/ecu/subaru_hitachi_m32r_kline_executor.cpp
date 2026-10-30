#include "src/backend/flash/ecu/subaru_hitachi_m32r_kline_executor.h"

#include <array>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/m32r_ssm_exchange.h"
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
    return ExchangeM32rSsmOptional(transport, cancellation, payload, p.tester_id, p.target_id,
                                   std::chrono::milliseconds{timeout});
}

M32rSsmEndpoint EndpointOf(const SubaruHitachiM32rKlinePlan& p)
{
    return M32rSsmEndpoint{p.tester_id, p.target_id, M32rSsmPeer::kEcu};
}

Result<bytes::Bytes> Exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruHitachiM32rKlinePlan& p)
{
    return ExchangeM32rSsmRequired(transport, cancellation, payload, EndpointOf(p),
                                   std::chrono::milliseconds{kTimeoutMs});
}

Status ExpectPrefix(bytes::ByteView response, std::initializer_list<bytes::Byte> prefix)
{
    return ExpectM32rSsmPrefix(response, prefix, M32rSsmPeer::kEcu);
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
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kStockPairTable);
}

bytes::Bytes Encrypt(bytes::ByteView image)
{
    static constexpr std::array<std::uint16_t, 4> kIndex = {0x78f1, 0x2962, 0x9312, 0x7c03};
    return ssm_protocol::CalculatePayload(image, kIndex, ssm_protocol::kStockPairTable);
}

Status RequestPrefix(IKlineFlashTransport& transport, const ICancellationToken& cancellation, bytes::Bytes request,
                     std::initializer_list<bytes::Byte> expected, const SubaruHitachiM32rKlinePlan& p)
{
    return RequestM32rSsmPrefix(transport, cancellation, request, EndpointOf(p), expected,
                                std::chrono::milliseconds{kTimeoutMs});
}

Status AuthenticatedSession(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                            const SubaruHitachiM32rKlinePlan& p, bool include_bf)
{
    if (include_bf)
    {
        if (auto s = Exchange(transport, cancellation, bytes::Bytes{0xbf}, p)
                         .and_then([](const bytes::Bytes& id) { return ParseRomId(id); })
                         .and_then([&](const std::string&)
                                   { return RequestPrefix(transport, cancellation, {0x81}, {0xc1}, p); });
            !s.has_value())
        {
            return s;
        }
    }
    return RequestPrefix(transport, cancellation, {0x83, 0}, {0xc3}, p)
        .and_then(
            [&]
            {
                return Exchange(transport, cancellation,
                                bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p);
            })
        .and_then(
            [&](const bytes::Bytes& seed)
            {
                return ExpectPrefix(seed, {0x67, uds::kSecurityAccessRequestSeed})
                    .and_then(
                        [&]() -> Status
                        {
                            if (seed.size() < 10)
                            {
                                return Fail(ErrorKind::kBadResponse, "seed response is too short");
                            }
                            bytes::Bytes key_request = ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey,
                                                                 SeedKey(bytes::ByteView{seed}.subspan(6, 4)));
                            return p.session_mode == HitachiM32rKlineSessionMode::kRecovery
                                       ? RequestPrefix(transport, cancellation, std::move(key_request), {0x67}, p)
                                       : RequestPrefix(transport, cancellation, std::move(key_request),
                                                       {0x67, uds::kSecurityAccessSendKey}, p);
                        });
            })
        .and_then(
            [&]
            {
                return RequestPrefix(transport, cancellation, {uds::kSidDiagnosticSessionControl, 0x85, 0x02}, {0x50},
                                     p);
            });
}

Result<std::string> PrepareRead(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                const SubaruHitachiM32rKlinePlan& p)
{
    auto probe = transport.SetBaud(p.read_baud)
                     .and_then([&] { return ExchangeOptional(transport, cancellation, bytes::Bytes{0xbf}, p); });
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
    return transport.SetBaud(p.initial_baud)
        .and_then([&] { return Exchange(transport, cancellation, bytes::Bytes{0xbf}, p); })
        .and_then([](const bytes::Bytes& initial) { return ParseRomId(initial); })
        .and_then(
            [&](std::string id)
            {
                return RequestPrefix(transport, cancellation, {0xb8, 0x00, 0x00, 0x00, 0x75}, {0xf8}, p)
                    .and_then([&] { return transport.SetBaud(p.read_baud); })
                    .and_then([&] { return RequestPrefix(transport, cancellation, {0xbf}, {0xff}, p); })
                    .transform([&] { return std::move(id); });
            });
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
    auto probe =
        transport.SetBaud(p.write_baud)
            .and_then(
                [&]
                {
                    return ExchangeOptional(transport, cancellation,
                                            bytes::Bytes{uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0}, p);
                });
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (*probe && (*probe)->size() >= 6 && (**probe)[4] == 0x74 && (**probe)[5] == 0x84)
    {
        return {};
    }
    return transport.SetBaud(p.initial_baud)
        .and_then([&] { return AuthenticatedSession(transport, cancellation, p, true); });
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
                                 ComposeBe(0xa0_b, 0x00_b, 0x00_b, U24{address}, bytes::Byte(p.chunk_size - 1)), p);
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

// Transfers the encrypted 512 KiB ROM in chunk_size blocks. A missing ack
// is tolerated; a present one must acknowledge.
Status TransferRom(IKlineFlashTransport& transport, const ICancellationToken& cancellation, IEventSink& events,
                   const SubaruHitachiM32rKlinePlan& p, const FlashPlan& plan)
{
    const bytes::Bytes encrypted = Encrypt(plan.ImageOrEmpty());
    for (std::uint32_t address = 0; address < 0x80000; address += p.chunk_size)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM write");
        }
        const bytes::Bytes request =
            ComposeBe(uds::kSidTransferData, U24{address}, bytes::ByteView(encrypted).subspan(address, p.chunk_size));
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
    return {};
}

Status WriteRom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                IEventSink& events, const SubaruHitachiM32rKlinePlan& p, const FlashPlan& plan)
{
    return transport.SetBaud(p.write_baud)
        .and_then(
            [&]
            {
                return RequestPrefix(transport, cancellation, {uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x08, 0, 0},
                                     {0x74}, p);
            })
        .and_then([&] { return clock.Sleep(200ms, cancellation); })
        .and_then([&] { return EraseRom(transport, clock, cancellation, p); })
        .and_then(
            [&]() -> Status
            {
                if (cancellation.Cancelled())
                {
                    return Fail(ErrorKind::kCancelled, "cancelled after erase");
                }
                return TransferRom(transport, cancellation, events, p, plan);
            })
        .and_then([&] { return clock.Sleep(300ms, cancellation); })
        .and_then(
            [&]
            {
                return ExchangeOptional(transport, cancellation,
                                        bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02}, p);
            })
        .and_then(
            [](const std::optional<bytes::Bytes>& checksum) -> Status
            {
                if (!checksum.has_value() || checksum->empty())
                {
                    return Fail(ErrorKind::kTimeout, "no final checksum response");
                }
                return {};
            });
}
} // namespace

Result<KlineConfig> SubaruHitachiM32rKlineExecutor::Setup(const SubaruHitachiM32rKlinePlan& family) const
{
    return NonIso14230KlineConfigFrom(family);
}

Result<FlashExecutionResult> SubaruHitachiM32rKlineExecutor::Run(const SubaruHitachiM32rKlinePlan& p,
                                                                 const FlashPlan& plan, IKlineFlashTransport& transport,
                                                                 IClock& clock, const ICancellationToken& cancellation,
                                                                 IEventSink& events)
{
    return transport.SetAddIso14230Header(false).and_then(
        [&]() -> Result<FlashExecutionResult>
        {
            if (plan.Operation() == FlashOperation::kRead)
            {
                return PrepareRead(transport, cancellation, p)
                    .and_then(
                        [&](std::string id)
                        {
                            return ReadRom(transport, cancellation, events, p)
                                .transform(
                                    [&](bytes::Bytes rom)
                                    {
                                        return FlashExecutionResult{FlashOperation::kRead, std::move(rom),
                                                                    std::move(id)};
                                    });
                        });
            }
            return PrepareWrite(transport, cancellation, p)
                .and_then([&] { return WriteRom(transport, clock, cancellation, events, p, plan); })
                .transform([] { return FlashExecutionResult{FlashOperation::kWrite, std::nullopt, std::nullopt}; });
        });
}
} // namespace fastecu::flash
