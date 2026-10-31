#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_types.h"

#include <array>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/m32r_ssm_exchange.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/logged_step.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

Result<std::optional<bytes::Bytes>> ExchangeOptional(IKlineFlashTransport& transport,
                                                     const ICancellationToken& cancellation, bytes::ByteView payload,
                                                     const SubaruMitsuM32rKlinePlan& p)
{
    return ExchangeM32rSsmOptional(transport, cancellation, payload, p.tester_id, p.target_id, 2000ms);
}

Result<bytes::Bytes> Exchange(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruMitsuM32rKlinePlan& p)
{
    return ExchangeM32rSsmRequired(transport, cancellation, payload,
                                   M32rSsmEndpoint{p.tester_id, p.target_id, M32rSsmPeer::kEcu}, 2000ms);
}

Status ExpectService(bytes::ByteView response, std::initializer_list<bytes::Byte> service)
{
    if (response.size() < 4 + service.size())
    {
        return Fail(ErrorKind::kBadResponse, "No valid response from ECU");
    }
    std::size_t i = 4;
    for (const bytes::Byte value : service)
    {
        if (response[i++] != value)
        {
            return Fail(ErrorKind::kBadResponse, "Wrong response from ECU");
        }
    }
    return {};
}

bytes::Bytes SeedKey(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex = {0x8519, 0x5c53, 0xc0e9, 0x2452, 0x1e68, 0x6feb,
                                                             0x2648, 0x81e2, 0x8ce4, 0x953b, 0x1ca9, 0x6180,
                                                             0xb85e, 0x5109, 0xdb3c, 0x3cf2};
    return ssm_protocol::CalculateSeedKey(seed, kIndex, ssm_protocol::kStockPairTable);
}

bytes::Bytes Encrypt(bytes::ByteView image)
{
    static constexpr std::array<std::uint16_t, 4> kIndex = {0x25b5, 0x3875, 0xca11, 0x2680};
    return ssm_protocol::CalculatePayload(image, kIndex, ssm_protocol::kStockPairTable);
}

Result<std::string> Handshake(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                              IEventSink& events, const SubaruMitsuM32rKlinePlan& p)
{
    const auto request = [&](bytes::Bytes payload, std::initializer_list<bytes::Byte> expected)
    {
        return Exchange(transport, cancellation, payload, p)
            .and_then([&](const bytes::Bytes& response) { return ExpectService(response, expected); });
    };
    return clock.Sleep(100ms, cancellation)
        .and_then(LogThen(events, "Requesting ECU ID",
                          [&] { return Exchange(transport, cancellation, bytes::Bytes{0xbf}, p); }))
        .and_then(
            [&](const bytes::Bytes& id)
            {
                return ExpectService(id, {0xff})
                    .and_then(
                        [&]() -> Result<std::string>
                        {
                            if (id.size() < 14)
                            {
                                return Fail(ErrorKind::kBadResponse, "ECU ID response does not contain five ID bytes");
                            }
                            std::string rom_id;
                            for (std::size_t i = 8; i < 13; ++i)
                            {
                                rom_id += std::format("{:02X}", id[i]);
                            }
                            rom_id += '_';
                            events.Log(LogLevel::kInfo, std::format("ECU ID: {}", rom_id.substr(0, rom_id.size() - 1)));
                            return rom_id;
                        });
            })
        .and_then(
            [&](std::string rom_id)
            {
                events.Log(LogLevel::kInfo, "Requesting to start communication");
                return request({0x81}, {0xc1})
                    .and_then(LogThen(events, "Start communication ok", "Requesting timings params",
                                      [&] { return request({0x83, 0x00}, {0xc3}); }))
                    .and_then(LogThen(
                        events, "Timing parameters ok", "Requesting seed",
                        [&]
                        {
                            return Exchange(transport, cancellation,
                                            bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed}, p);
                        }))
                    .and_then(
                        [&](const bytes::Bytes& seed_response)
                        {
                            return ExpectService(seed_response, {0x67, uds::kSecurityAccessRequestSeed})
                                .and_then(
                                    [&]() -> Status
                                    {
                                        if (seed_response.size() < 10)
                                        {
                                            return Fail(ErrorKind::kBadResponse, "seed response is too short");
                                        }
                                        bytes::Bytes key_request =
                                            ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey,
                                                      SeedKey(bytes::ByteView{seed_response}.subspan(6, 4)));
                                        events.Log(LogLevel::kInfo, "Sending seed key to ECU");
                                        return request(std::move(key_request), {0x67, uds::kSecurityAccessSendKey});
                                    });
                        })
                    .and_then(LogThen(events, "Seed key ok", "Set session mode",
                                      [&] { return request({uds::kSidDiagnosticSessionControl, 0x85, 0x02}, {0x50}); }))
                    .transform(
                        [&]
                        {
                            events.Log(LogLevel::kInfo, "Succesfully set to programming session");
                            return std::move(rom_id);
                        });
            });
}

Result<bytes::Bytes> ReadRom(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                             IEventSink& events, const SubaruMitsuM32rKlinePlan& p, MemoryRegion region)
{
    bytes::Bytes rom(region.start, p.unread_prefix_fill);
    rom.reserve(region.start + region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += p.chunk_size)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = region.start + offset;
        const auto request = ComposeBe(0xa0_b, 0x00_b, 0x20_b, U24{address}, bytes::Byte(p.chunk_size - 1));
        auto response = Exchange(transport, cancellation, request, p);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->size() != p.chunk_size + 6 || (*response)[4] != 0xe0)
        {
            return Fail(ErrorKind::kBadResponse, "ROM read response must contain exactly 128 data bytes");
        }
        rom.insert(rom.end(), response->begin() + 5, response->end() - 1);
        events.Progress(static_cast<int>(offset + p.chunk_size), static_cast<int>(region.length));
    }
    events.Log(LogLevel::kWarning, "The first 0x8000 ROM bytes are synthetic 0xFF; this protocol reads userspace only");
    return rom;
}

Status WriteRom(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                IEventSink& events, const SubaruMitsuM32rKlinePlan& p, const FlashPlan& plan)
{
    // Appending below deliberately replaces the legacy out-of-bounds
    // operator[] construction while preserving the transmitted bytes.
    const Status erased =
        transport.SetBaud(p.flash_baud)
            .and_then(
                [&]
                {
                    return Exchange(transport, cancellation,
                                    bytes::Bytes{uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x07, 0x80, 0}, p);
                })
            .and_then([](const bytes::Bytes& setup) { return ExpectService(setup, {0x74}); })
            .and_then(LogThen(events, "Erasing...",
                              [&]
                              {
                                  return Exchange(transport, cancellation,
                                                  bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x0f,
                                                               0xff, 0xff, 0xff},
                                                  p);
                              }))
            .and_then([](const bytes::Bytes& erase_reply) { return ExpectService(erase_reply, {0x71}); })
            .and_then([&] { return CheckCancelled(cancellation, "cancelled after erase"); });
    if (!erased.has_value())
    {
        return erased;
    }

    const bytes::Bytes encrypted = Encrypt(plan.ImageOrEmpty());
    const MemoryRegion region = plan.TransferRegion();
    events.Log(LogLevel::kInfo, "Starting ROM Flashing...");
    for (std::uint32_t offset = 0; offset < region.length; offset += p.chunk_size)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM write");
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request =
            ComposeBe(uds::kSidTransferData, U24{address}, bytes::ByteView(encrypted).subspan(address, p.chunk_size));
        if (auto ack = ExchangeOptional(transport, cancellation, request, p); !ack.has_value())
        {
            return std::unexpected(ack.error());
        }
        events.Progress(static_cast<int>(offset + p.chunk_size), static_cast<int>(region.length));
    }
    events.Log(LogLevel::kInfo, "Verifying checksum...");
    return clock.Sleep(1000ms, cancellation)
        .and_then(
            [&]
            {
                return Exchange(transport, cancellation,
                                bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02}, p);
            })
        .and_then([](const bytes::Bytes& checksum)
                  { return ExpectService(checksum, {0x71, uds::kRoutineControlStart, 0x02}); })
        .transform([&] { events.Log(LogLevel::kInfo, "Checksum verified..."); });
}

Result<FlashExecutionResult> ExecuteOpenTransport(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                                  const ICancellationToken& cancellation, IEventSink& events,
                                                  const SubaruMitsuM32rKlinePlan& parameters)
{
    return transport.SetAddIso14230Header(false)
        .and_then(LogThen(events, "Connecting to ECU K-Line bootloader, please wait...",
                          [&] { return Handshake(transport, clock, cancellation, events, parameters); }))
        .and_then(
            [&](std::string rom_id) -> Result<FlashExecutionResult>
            {
                if (plan.Operation() == FlashOperation::kRead)
                {
                    events.Log(LogLevel::kInfo, "Reading ROM from ECU using K-Line");
                    return ReadRom(transport, cancellation, events, parameters, plan.TransferRegion())
                        .transform(
                            [&](bytes::Bytes rom)
                            { return FlashExecutionResult{plan.Operation(), std::move(rom), std::move(rom_id)}; });
                }
                events.Log(LogLevel::kInfo, "Writing ROM to ECU using K-Line");
                return WriteRom(transport, clock, cancellation, events, parameters, plan)
                    .transform([&] { return FlashExecutionResult{plan.Operation(), std::nullopt, std::nullopt}; });
            });
}
} // namespace

Result<KlineConfig> SubaruMitsuM32rKlineExecutor::Setup(const SubaruMitsuM32rKlinePlan& family) const
{
    return NonIso14230KlineConfigFrom(family);
}

Result<FlashExecutionResult> SubaruMitsuM32rKlineExecutor::Run(const SubaruMitsuM32rKlinePlan& p, const FlashPlan& plan,
                                                               IKlineFlashTransport& transport, IClock& clock,
                                                               const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    return ExecuteOpenTransport(plan, transport, clock, cancellation, events, p);
}
} // namespace fastecu::flash
