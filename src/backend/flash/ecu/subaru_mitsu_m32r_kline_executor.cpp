#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_executor.h"

#include <array>
#include <format>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/subaru_mitsu_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;

constexpr int kTimeoutMs = 2000;

bytes::Bytes framed(bytes::ByteView payload, const SubaruMitsuM32rKlinePlan& p)
{
    return SsmProtocol::addHeader(payload, p.tester_id, p.target_id);
}

Result<std::optional<bytes::Bytes>> exchange_optional(IKlineFlashTransport& transport,
                                                      const ICancellationToken& cancellation,
                                                      bytes::ByteView payload,
                                                      const SubaruMitsuM32rKlinePlan& p)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before write");
    }
    auto written = transport.write(framed(payload, p));
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != payload.size() + 5)
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after write");
    }
    auto response = transport.read(kTimeoutMs, cancellation);
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

Result<bytes::Bytes> exchange(IKlineFlashTransport& transport,
                              const ICancellationToken& cancellation,
                              bytes::ByteView payload, const SubaruMitsuM32rKlinePlan& p)
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

Status expect_service(bytes::ByteView response, std::initializer_list<bytes::Byte> service)
{
    if (response.size() < 4 + service.size())
    {
        return fail(ErrorKind::BadResponse, "No valid response from ECU");
    }
    std::size_t i = 4;
    for (const bytes::Byte value : service)
    {
        if (response[i++] != value)
        {
            return fail(ErrorKind::BadResponse, "Wrong response from ECU");
        }
    }
    return {};
}

bytes::Bytes seed_key(bytes::ByteView seed)
{
    static constexpr std::array<std::uint16_t, 16> kIndex = {0x8519, 0x5c53, 0xc0e9, 0x2452,
                                                             0x1e68, 0x6feb, 0x2648, 0x81e2, 0x8ce4, 0x953b, 0x1ca9, 0x6180,
                                                             0xb85e, 0x5109, 0xdb3c, 0x3cf2};
    static constexpr std::array<std::uint8_t, 32> kTransform = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8,
                                                                0xa, 0xd, 0x2, 0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2, 0xd, 0x9,
                                                                0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculateSeedKey(seed, kIndex, kTransform);
}

bytes::Bytes encrypt(bytes::ByteView image)
{
    static constexpr std::array<std::uint16_t, 4> kIndex = {0x25b5, 0x3875, 0xca11, 0x2680};
    static constexpr std::array<std::uint8_t, 32> kTransform = {0x5, 0x6, 0x7, 0x1, 0x9, 0xc, 0xd, 0x8,
                                                                0xa, 0xd, 0x2, 0xb, 0xf, 0x4, 0x0, 0x3, 0xb, 0x4, 0x6, 0x0, 0xf, 0x2, 0xd, 0x9,
                                                                0x5, 0xc, 0x1, 0xa, 0x3, 0xd, 0xe, 0x8};
    return SsmProtocol::calculatePayload(image, static_cast<std::uint32_t>(image.size()),
                                         kIndex, kTransform);
}

Result<std::string> handshake(IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation, IEventSink& events,
                              const SubaruMitsuM32rKlinePlan& p)
{
    // Legacy wire sequence and delays: flash_ecu_subaru_mitsu_m32r_kline_operation.cpp
    // connect_bootloader() lines 85-248 and send_sid_bf/81/83/27/10 lines 691-815.
    if (auto slept = clock.sleep(100, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    events.log(LogLevel::Info, "Requesting ECU ID");
    auto id = exchange(transport, cancellation, bytes::Bytes{0xbf}, p);
    if (!id.has_value())
    {
        return std::unexpected(id.error());
    }
    if (auto valid = expect_service(*id, {0xff}); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (id->size() < 14)
    {
        return fail(ErrorKind::BadResponse, "ECU ID response does not contain five ID bytes");
    }
    std::string rom_id;
    for (std::size_t i = 8; i < 13; ++i)
    {
        rom_id += std::format("{:02X}", (*id)[i]);
    }
    rom_id += '_';
    events.log(LogLevel::Info, std::format("ECU ID: {}", rom_id.substr(0, rom_id.size() - 1)));

    const auto request = [&](bytes::Bytes payload, std::initializer_list<bytes::Byte> expected) -> Status
    {
        auto response = exchange(transport, cancellation, payload, p);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        return expect_service(*response, expected);
    };
    events.log(LogLevel::Info, "Requesting to start communication");
    if (auto s = request({0x81}, {0xc1}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    events.log(LogLevel::Info, "Start communication ok");
    events.log(LogLevel::Info, "Requesting timings params");
    if (auto s = request({0x83, 0x00}, {0xc3}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    events.log(LogLevel::Info, "Timing parameters ok");
    events.log(LogLevel::Info, "Requesting seed");
    auto seed_response = exchange(
        transport, cancellation, bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed},
        p);
    if (!seed_response.has_value())
    {
        return std::unexpected(seed_response.error());
    }
    if (auto s = expect_service(*seed_response, {0x67, uds::kSecurityAccessRequestSeed});
        !s.has_value())
    {
        return std::unexpected(s.error());
    }
    if (seed_response->size() < 10)
    {
        return fail(ErrorKind::BadResponse, "seed response is too short");
    }
    const bytes::Bytes seed(seed_response->begin() + 6, seed_response->begin() + 10);
    const bytes::Bytes key = seed_key(seed);
    bytes::Bytes key_request{uds::kSidSecurityAccess, uds::kSecurityAccessSendKey};
    key_request.insert(key_request.end(), key.begin(), key.end());
    events.log(LogLevel::Info, "Sending seed key to ECU");
    if (auto s = request(std::move(key_request), {0x67, uds::kSecurityAccessSendKey}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    events.log(LogLevel::Info, "Seed key ok");
    events.log(LogLevel::Info, "Set session mode");
    if (auto s = request({uds::kSidDiagnosticSessionControl, 0x85, 0x02}, {0x50}); !s.has_value())
    {
        return std::unexpected(s.error());
    }
    events.log(LogLevel::Info, "Succesfully set to programming session");
    return rom_id;
}

Result<bytes::Bytes> read_rom(IKlineFlashTransport& transport,
                              const ICancellationToken& cancellation, IEventSink& events,
                              const SubaruMitsuM32rKlinePlan& p, MemoryRegion region)
{
    // Legacy SID A0 request and 128-byte loop: operation.cpp lines 250-344 and 665-689.
    bytes::Bytes rom(region.start, p.unread_prefix_fill);
    rom.reserve(region.start + region.length);
    for (std::uint32_t offset = 0; offset < region.length; offset += p.chunk_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM read");
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request = composeBe(0xa0_b, 0x00_b, 0x20_b, u24(address),
                                               bytes::Byte(p.chunk_size - 1));
        auto response = exchange(transport, cancellation, request, p);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->size() != p.chunk_size + 6 || (*response)[4] != 0xe0)
        {
            return fail(ErrorKind::BadResponse, "ROM read response must contain exactly 128 data bytes");
        }
        rom.insert(rom.end(), response->begin() + 5, response->end() - 1);
        events.progress(static_cast<int>(offset + p.chunk_size), static_cast<int>(region.length));
    }
    events.log(LogLevel::Warning,
               "The first 0x8000 ROM bytes are synthetic 0xFF; this protocol reads userspace only");
    return rom;
}

Status write_rom(IKlineFlashTransport& transport, IClock& clock,
                 const ICancellationToken& cancellation, IEventSink& events,
                 const SubaruMitsuM32rKlinePlan& p, const FlashPlan& plan)
{
    // Legacy baud switch, SID 34/31/36 traffic, acknowledgement tolerance, and checksum:
    // operation.cpp lines 425-641. Appending below deliberately replaces the legacy
    // out-of-bounds operator[] construction while preserving the transmitted bytes.
    if (auto baud = transport.setBaud(p.flash_baud); !baud.has_value())
    {
        return baud;
    }
    auto setup = exchange(
        transport, cancellation,
        bytes::Bytes{uds::kSidRequestDownload, 0, 0, 0, 0x04, 0x07, 0x80, 0}, p);
    if (!setup.has_value())
    {
        return std::unexpected(setup.error());
    }
    if (auto s = expect_service(*setup, {0x74}); !s.has_value())
    {
        return s;
    }
    events.log(LogLevel::Info, "Erasing...");
    auto erased = exchange(
        transport, cancellation,
        bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStop, 0x0f, 0xff, 0xff, 0xff}, p);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    if (auto s = expect_service(*erased, {0x71}); !s.has_value())
    {
        return s;
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled after erase");
    }

    const bytes::Bytes encrypted = encrypt(*plan.image());
    const MemoryRegion region = plan.transfer_region();
    events.log(LogLevel::Info, "Starting ROM Flashing...");
    for (std::uint32_t offset = 0; offset < region.length; offset += p.chunk_size)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled during ROM write");
        }
        const std::uint32_t address = region.start + offset;
        const bytes::Bytes request = composeBe(
            uds::kSidTransferData, u24(address),
            bytes::ByteView(encrypted).subspan(address, p.chunk_size));
        if (auto ack = exchange_optional(transport, cancellation, request, p); !ack.has_value())
        {
            return std::unexpected(ack.error());
        }
        events.progress(static_cast<int>(offset + p.chunk_size), static_cast<int>(region.length));
    }
    events.log(LogLevel::Info, "Verifying checksum...");
    if (auto slept = clock.sleep(1000, cancellation); !slept.has_value())
    {
        return slept;
    }
    auto checksum = exchange(
        transport, cancellation,
        bytes::Bytes{uds::kSidRoutineControl, uds::kRoutineControlStart, 0x02}, p);
    if (!checksum.has_value())
    {
        return std::unexpected(checksum.error());
    }
    if (auto s = expect_service(*checksum, {0x71, uds::kRoutineControlStart, 0x02}); !s.has_value())
    {
        return s;
    }
    events.log(LogLevel::Info, "Checksum verified...");
    return {};
}

Result<FlashExecutionResult> execute_open_transport(
    const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events,
    const SubaruMitsuM32rKlinePlan& parameters)
{
    if (auto header = transport.set_add_iso14230_header(false); !header.has_value())
    {
        return std::unexpected(header.error());
    }
    events.log(LogLevel::Info, "Connecting to ECU K-Line bootloader, please wait...");
    auto rom_id = handshake(transport, clock, cancellation, events, parameters);
    if (!rom_id.has_value())
    {
        return std::unexpected(rom_id.error());
    }
    if (plan.operation() == FlashOperation::Read)
    {
        events.log(LogLevel::Info, "Reading ROM from ECU using K-Line");
        auto bytes = read_rom(transport, cancellation, events, parameters, plan.transfer_region());
        if (!bytes.has_value())
        {
            return std::unexpected(bytes.error());
        }
        return FlashExecutionResult{plan.operation(), std::move(*bytes), std::move(*rom_id)};
    }
    events.log(LogLevel::Info, "Writing ROM to ECU using K-Line");
    if (auto status = write_rom(transport, clock, cancellation, events, parameters, plan);
        !status.has_value())
    {
        return std::unexpected(status.error());
    }
    return FlashExecutionResult{plan.operation(), std::nullopt, std::nullopt};
}
} // namespace

Result<FlashExecutionResult> SubaruMitsuM32rKlineExecutor::execute(
    const FlashPlan& plan, IFlashTransport& transport, IClock& clock,
    const ICancellationToken& cancellation, IEventSink& events)
{
    if (auto match = check_family_transport_match(plan, FlashFamily::SubaruMitsuM32rKline,
                                                  TransportKind::Kline);
        !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (auto valid = validate_subaru_mitsu_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, "cancelled before setup");
    }
    auto *kline = dynamic_cast<IKlineFlashTransport *>(&transport);
    if (!kline)
    {
        return fail(ErrorKind::InvalidConfig, "transport does not implement IKlineFlashTransport");
    }
    const auto& p = std::get<SubaruMitsuM32rKlinePlan>(plan.family_plan());
    if (auto configured = kline->configure({p.initial_baud, false, p.tester_id, p.target_id});
        !configured.has_value())
    {
        return std::unexpected(configured.error());
    }
    if (auto opened = kline->open(); !opened.has_value())
    {
        return std::unexpected(opened.error());
    }
    Result<FlashExecutionResult> outcome = execute_open_transport(
        plan, *kline, clock, cancellation, events, p);
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
