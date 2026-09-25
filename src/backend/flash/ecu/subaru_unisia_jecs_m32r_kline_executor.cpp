#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_executor.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::composeBe;
using bytes::u24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// Legacy header timeouts (flash_ecu_subaru_unisia_jecs_m32r_operation.h).
constexpr auto kTimeout = 2000ms;          // serial_read_timeout
constexpr auto kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout
constexpr auto kPagePacing = 1ms;          // read_mem() :314
constexpr std::uint32_t kPage = 0x80;      // read_mem() :220, write_mem() :523
constexpr int kColdBaud = 4800;            // read_mem() :141, write_mem() :375
constexpr int kReadBaud = 38400;           // read_mem() :102, :195
// The ECU ID is the five bytes after the header, SID and three capability
// bytes of the BF reply (read_mem() :162-163).
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;

struct Session
{
    IKlineFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
    const SubaruUnisiaJecsM32rKlinePlan& wire;
};

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

// Legacy write_serial_data_echo_check() of an SsmProtocol::addHeader frame.
Status send(Session& s, bytes::ByteView payload)
{
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before write"); !cancelled.has_value())
    {
        return cancelled;
    }
    const bytes::Bytes request = SsmProtocol::addHeader(payload, s.wire.tester_id, s.wire.target_id);
    auto written = s.transport.write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return fail(ErrorKind::Disconnected, "short K-Line write");
    }
    return {};
}

// One framed read; an empty optional means no frame arrived in time.
Result<std::optional<bytes::Bytes>> receive(Session& s, std::chrono::milliseconds timeout)
{
    auto response = s.transport.read(timeout, s.cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (Status cancelled = cancelled_if_requested(s.cancellation, "after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return std::move(*response);
}

bool has_sid(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::Byte sid)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] >= 1 && frame[4] == sid;
}

bool carries_ecu_id(bytes::ByteView frame)
{
    return frame.size() >= kEcuIdOffset + kEcuIdLength + 1;
}

// Sends `payload` and requires a valid frame whose first payload byte is `sid`.
Result<bytes::Bytes> exchange_expect(Session& s, bytes::ByteView payload, std::chrono::milliseconds timeout,
                                     bytes::Byte sid, std::string_view what)
{
    if (Status sent = send(s, payload); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto response = receive(s, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return fail(ErrorKind::Timeout, std::format("no response to {}", what));
    }
    if (!has_sid(**response, s.wire, sid))
    {
        return fail(ErrorKind::BadResponse,
                    std::format("unexpected response to {}: {}", what, bytes::toHex(**response)));
    }
    return std::move(**response);
}

// send_sid_bf_ssm_init() :637-650, gated: the reply must be FF and long
// enough to carry the ECU ID.
Result<bytes::Bytes> expect_ssm_init(Session& s)
{
    auto init = exchange_expect(s, composeBe(0xbf_b), kTimeout, 0xff, "SSM init");
    if (!init.has_value())
    {
        return init;
    }
    if (!carries_ecu_id(*init))
    {
        return fail(ErrorKind::BadResponse, std::format("SSM init reply carries no ECU ID: {}", bytes::toHex(*init)));
    }
    return init;
}

std::string ecu_id(Session& s, bytes::ByteView init)
{
    std::string id;
    for (const bytes::Byte value : init.subspan(kEcuIdOffset, kEcuIdLength))
    {
        id += std::format("{:02X}", value);
    }
    s.events.log(LogLevel::Info, std::format("ECU ID: {}", id));
    return id;
}

Result<FlashExecutionResult> read_rom(Session& s, const FlashPlan& plan)
{
    // read_mem() :101-104: probe at 38400 in case the ECU is already in read
    // mode. A mismatch is logged and the cold init follows, as legacy did.
    if (Status baud = s.transport.setBaud(kReadBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    s.events.log(LogLevel::Info, "Checking if ECU in read mode");
    if (Status sent = send(s, composeBe(0xbf_b)); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto probe = receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    std::optional<bytes::Bytes> init;
    if (probe->has_value() && has_sid(**probe, s.wire, 0xff) && carries_ecu_id(**probe))
    {
        init = std::move(**probe);
    }
    else
    {
        s.events.log(LogLevel::Info, "Read mode not active, initialising ECU...");
        // read_mem() :141-216.
        if (Status baud = s.transport.setBaud(kColdBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        auto cold = expect_ssm_init(s);
        if (!cold.has_value())
        {
            return std::unexpected(cold.error());
        }
        init = std::move(*cold);
        // send_sid_b8_change_baudrate_38400() :671-688.
        auto changed =
            exchange_expect(s, composeBe(0xb8_b, 0x00_b, 0x00_b, 0x00_b, 0x75_b), kTimeout, 0xf8, "baud rate change");
        if (!changed.has_value())
        {
            return std::unexpected(changed.error());
        }
        if (Status baud = s.transport.setBaud(kReadBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        s.events.log(LogLevel::Info, "Requesting ECU ID, checking if baudrate change was ok");
        auto confirmed = expect_ssm_init(s);
        if (!confirmed.has_value())
        {
            return std::unexpected(confirmed.error());
        }
    }
    const std::string id = ecu_id(s, *init);

    // read_mem() :219-318. Every page must be a complete, checksummed E0
    // frame carrying exactly one page; legacy appended any E0 reply.
    const MemoryRegion region = plan.transfer_region();
    const auto pages = static_cast<int>(region.length / kPage);
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (int page = 0; page < pages; ++page)
    {
        const std::uint32_t address = region.start + static_cast<std::uint32_t>(page) * kPage;
        auto block = exchange_expect(s, composeBe(0xa0_b, 0x00_b, u24(address), bytes::Byte(kPage - 1)),
                                     kExtraLongTimeout, 0xe0, std::format("block read at 0x{:06X}", address));
        if (!block.has_value())
        {
            return std::unexpected(block.error());
        }
        if (block->size() != 4 + 1 + kPage + 1)
        {
            return fail(ErrorKind::BadResponse,
                        std::format("block read at 0x{:06X} returned {} bytes", address, block->size()));
        }
        rom.insert(rom.end(), block->begin() + 5, block->begin() + 5 + kPage);
        s.events.progress(page + 1, pages);
        if (Status paced = s.clock.sleep(kPagePacing, s.cancellation); !paced.has_value())
        {
            return std::unexpected(paced.error());
        }
    }
    return FlashExecutionResult{.operation = FlashOperation::Read, .read_bytes = std::move(rom), .rom_id = id + "_"};
}

Status write_rom(Session&, const FlashPlan&)
{
    return fail(ErrorKind::Unsupported, "Unisia Jecs M32R write is not implemented yet");
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rKlineExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :50-57.
    return non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.family_plan()));
}

Status SubaruUnisiaJecsM32rKlineExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                     const ICancellationToken&) const
{
    // execute() :50. Clears a header left enabled by an earlier session.
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rKlineExecutor::execute(const FlashPlan& plan,
                                                                        IKlineFlashTransport& transport, IClock& clock,
                                                                        const ICancellationToken& cancellation,
                                                                        IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_kline_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events,
                    std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.family_plan())};
    if (plan.operation() == FlashOperation::Read)
    {
        return read_rom(session, plan);
    }

    const Status written = write_rom(session, plan);
    // execute() :71-72: the LEC lines drop after write_mem() on every outcome.
    events.log(LogLevel::Debug, "Removing programming voltage +12v from Line End Check 1");
    const Status dropped = transport.disable_lec_lines();
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
