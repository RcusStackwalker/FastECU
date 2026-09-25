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
constexpr auto kMediumTimeout = 500ms;  // serial_read_medium_timeout
constexpr int kWriteBaud = 19200;       // write_mem() :347, :431
constexpr auto kErasePollSleep = 500ms; // write_mem() :475, :515
constexpr int kEraseStartRounds = 20;   // write_mem() :448
constexpr int kEraseDoneRounds = 40;    // write_mem() :488
constexpr bytes::Byte kBlockXor = 0x82; // write_mem() :525, :557

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

// The five ECU ID bytes of a gated SSM init reply, as uppercase hex.
std::string ecu_id_hex(bytes::ByteView init)
{
    std::string id;
    for (const bytes::Byte value : init.subspan(kEcuIdOffset, kEcuIdLength))
    {
        id += std::format("{:02X}", value);
    }
    return id;
}

// read_mem() and write_mem() both logged the ECU ID after SSM init.
void log_ecu_id(Session& s, std::string_view id)
{
    s.events.log(LogLevel::Info, std::format("ECU ID: {}", id));
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
    const std::string id = ecu_id_hex(*init);
    log_ecu_id(s, id);

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

bool is_exact_reply(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::ByteView payload)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == payload.size() &&
           std::ranges::equal(frame.subspan(4, payload.size()), payload);
}

// write_mem() :448-516. read() returns whole frames, so legacy's byte
// accumulation has no counterpart: an empty read continues the poll, and any
// frame returned must be exactly `expected`. Exhausting the rounds fails;
// legacy's second poll fell through to programming.
Status poll_for(Session& s, bytes::ByteView expected, int rounds, std::string_view what)
{
    for (int round = 0; round < rounds; ++round)
    {
        auto response = receive(s, kMediumTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!is_exact_reply(**response, s.wire, expected))
            {
                return fail(ErrorKind::BadResponse, std::format("{} failed: {}", what, bytes::toHex(**response)));
            }
            return {};
        }
        if (Status slept = s.clock.sleep(kErasePollSleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return fail(ErrorKind::Timeout, std::format("no {} response after {} polls", what, rounds));
}

// write_mem() :346-432. Returns once the ECU is in flash mode at 19200 baud.
Status enter_flash_mode(Session& s, std::uint32_t rom_size)
{
    if (Status baud = s.transport.setBaud(kWriteBaud); !baud.has_value())
    {
        return baud;
    }
    s.events.log(LogLevel::Info, "Checking if OBK is running");
    if (Status sent = send(s, composeBe(0xaf_b)); !sent.has_value())
    {
        return sent;
    }
    auto probe = receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && has_sid(**probe, s.wire, 0xef))
    {
        return {};
    }
    s.events.log(LogLevel::Info, "OBK not running, requesting flash mode");

    if (Status baud = s.transport.setBaud(kColdBaud); !baud.has_value())
    {
        return baud;
    }
    auto init = expect_ssm_init(s);
    if (!init.has_value())
    {
        return std::unexpected(init.error());
    }
    log_ecu_id(s, ecu_id_hex(*init));
    // send_sid_af_enter_flash_mode() :690-714. Gated: legacy logged a
    // rejection here and went on to raise VPP and erase.
    s.events.log(LogLevel::Info, "Sending request to change to flash mode");
    auto entered = exchange_expect(
        s, composeBe(0xaf_b, 0x11_b, bytes::ByteView(*init).subspan(kEcuIdOffset, kEcuIdLength), u24(rom_size)),
        kTimeout, 0xef, "enter flash mode");
    if (!entered.has_value())
    {
        return std::unexpected(entered.error());
    }
    s.events.log(LogLevel::Debug, "Changing baudrate to 19200");
    return s.transport.setBaud(kWriteBaud);
}

Status write_rom(Session& s, const FlashPlan& plan)
{
    const bytes::Bytes& image = *plan.image();
    const std::uint32_t rom_size = plan.transfer_region().length;
    if (Status entered = enter_flash_mode(s, rom_size); !entered.has_value())
    {
        return entered;
    }

    // write_mem() :440-441. The operator confirmed external VPP before the
    // run when the adapter cannot supply it.
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before programming voltage"); !cancelled.has_value())
    {
        return cancelled;
    }
    s.events.log(LogLevel::Debug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.enable_programming_voltage_line(); !raised.has_value())
    {
        return raised;
    }

    // send_sid_af_erase_memory_block() :716-731 sends without reading.
    s.events.log(LogLevel::Info, "Sending request to erase flash");
    if (Status sent = send(s, composeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status started = poll_for(s, composeBe(0xef_b, 0x42_b), kEraseStartRounds, "flash erase start");
        !started.has_value())
    {
        return started;
    }
    s.events.log(LogLevel::Info, "Flash erase in progress, please wait...");
    if (Status erased = poll_for(s, composeBe(0xef_b, 0x52_b), kEraseDoneRounds, "flash erase"); !erased.has_value())
    {
        return erased;
    }
    s.events.log(LogLevel::Info, "Flash erased!");
    // write_mem() :517: one more read, whose result legacy discarded.
    auto trailing = receive(s, kMediumTimeout);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        s.events.log(LogLevel::Debug, std::format("Discarded after erase: {}", bytes::toHex(**trailing)));
    }

    // write_mem() :535-625.
    const auto blocks = static_cast<int>(rom_size / kPage);
    const bytes::Bytes done = composeBe(0xef_b, 0x52_b);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kPage;
        const bool last = block == blocks - 1;
        bytes::Bytes data(image.begin() + address, image.begin() + address + kPage);
        for (bytes::Byte& value : data)
        {
            value ^= kBlockXor;
        }
        if (Status sent = send(s, composeBe(0xaf_b, last ? 0x69_b : 0x61_b, u24(address), bytes::ByteView(data)));
            !sent.has_value())
        {
            return sent;
        }
        auto response = receive(s, kExtraLongTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response->has_value())
        {
            if (!last)
            {
                return fail(ErrorKind::Timeout, std::format("no response to block write at 0x{:06X}", address));
            }
            // Legacy never read a reply to AF 69 (:564); its shape is unknown.
            s.events.log(LogLevel::Warning, "No reply to the final block; treating the write as complete");
        }
        else if (!is_exact_reply(**response, s.wire, done))
        {
            return fail(ErrorKind::BadResponse,
                        std::format("block write at 0x{:06X} failed: {}", address, bytes::toHex(**response)));
        }
        s.events.progress(block + 1, blocks);
    }
    s.events.log(LogLevel::Info, "ROM written to flash.");
    return {};
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
