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
using bytes::ComposeBe;
using bytes::U24;
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

Status CancelledIfRequested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::format("cancelled {}", where));
    }
    return {};
}

// Legacy write_serial_data_echo_check() of an SsmProtocol::addHeader frame.
Status Send(Session& s, bytes::ByteView payload)
{
    if (Status cancelled = CancelledIfRequested(s.cancellation, "before write"); !cancelled.has_value())
    {
        return cancelled;
    }
    const bytes::Bytes request = ssm_protocol::AddHeader(payload, s.wire.tester_id, s.wire.target_id);
    auto written = s.transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (*written != request.size())
    {
        return Fail(ErrorKind::kDisconnected, "short K-Line write");
    }
    return {};
}

// One framed read; an empty optional means no frame arrived in time.
Result<std::optional<bytes::Bytes>> Receive(Session& s, std::chrono::milliseconds timeout)
{
    auto response = s.transport.Read(timeout, s.cancellation);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (Status cancelled = CancelledIfRequested(s.cancellation, "after read"); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    return std::move(*response);
}

bool HasSid(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::Byte sid)
{
    return ssm_protocol::HasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] >= 1 && frame[4] == sid;
}

bool CarriesEcuId(bytes::ByteView frame)
{
    return frame.size() >= kEcuIdOffset + kEcuIdLength + 1;
}

// Sends `payload` and requires a valid frame whose first payload byte is `sid`.
Result<bytes::Bytes> ExchangeExpect(Session& s, bytes::ByteView payload, std::chrono::milliseconds timeout,
                                    bytes::Byte sid, std::string_view what)
{
    if (Status sent = Send(s, payload); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto response = Receive(s, timeout);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (!response->has_value())
    {
        return Fail(ErrorKind::kTimeout, std::format("no response to {}", what));
    }
    if (!HasSid(**response, s.wire, sid))
    {
        return Fail(ErrorKind::kBadResponse,
                    std::format("unexpected response to {}: {}", what, bytes::ToHex(**response)));
    }
    return std::move(**response);
}

// send_sid_bf_ssm_init() :637-650, gated: the reply must be FF and long
// enough to carry the ECU ID.
Result<bytes::Bytes> ExpectSsmInit(Session& s)
{
    auto init = ExchangeExpect(s, ComposeBe(0xbf_b), kTimeout, 0xff, "SSM init");
    if (!init.has_value())
    {
        return init;
    }
    if (!CarriesEcuId(*init))
    {
        return Fail(ErrorKind::kBadResponse, std::format("SSM init reply carries no ECU ID: {}", bytes::ToHex(*init)));
    }
    return init;
}

// The five ECU ID bytes of a gated SSM init reply, as uppercase hex.
std::string EcuIdHex(bytes::ByteView init)
{
    std::string id;
    for (const bytes::Byte value : init.subspan(kEcuIdOffset, kEcuIdLength))
    {
        id += std::format("{:02X}", value);
    }
    return id;
}

// read_mem() and write_mem() both logged the ECU ID after SSM init.
void LogEcuId(Session& s, std::string_view id)
{
    s.events.Log(LogLevel::kInfo, std::format("ECU ID: {}", id));
}

Result<FlashExecutionResult> ReadRom(Session& s, const FlashPlan& plan)
{
    // read_mem() :101-104: probe at 38400 in case the ECU is already in read
    // mode. A mismatch is logged and the cold init follows, as legacy did.
    if (Status baud = s.transport.SetBaud(kReadBaud); !baud.has_value())
    {
        return std::unexpected(baud.error());
    }
    s.events.Log(LogLevel::kInfo, "Checking if ECU in read mode");
    if (Status sent = Send(s, ComposeBe(0xbf_b)); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto probe = Receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    std::optional<bytes::Bytes> init;
    if (probe->has_value() && HasSid(**probe, s.wire, 0xff) && CarriesEcuId(**probe))
    {
        init = std::move(**probe);
    }
    else
    {
        s.events.Log(LogLevel::kInfo, "Read mode not active, initialising ECU...");
        // read_mem() :141-216.
        if (Status baud = s.transport.SetBaud(kColdBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        auto cold = ExpectSsmInit(s);
        if (!cold.has_value())
        {
            return std::unexpected(cold.error());
        }
        init = std::move(*cold);
        // send_sid_b8_change_baudrate_38400() :671-688.
        auto changed =
            ExchangeExpect(s, ComposeBe(0xb8_b, 0x00_b, 0x00_b, 0x00_b, 0x75_b), kTimeout, 0xf8, "baud rate change");
        if (!changed.has_value())
        {
            return std::unexpected(changed.error());
        }
        if (Status baud = s.transport.SetBaud(kReadBaud); !baud.has_value())
        {
            return std::unexpected(baud.error());
        }
        s.events.Log(LogLevel::kInfo, "Requesting ECU ID, checking if baudrate change was ok");
        auto confirmed = ExpectSsmInit(s);
        if (!confirmed.has_value())
        {
            return std::unexpected(confirmed.error());
        }
    }
    const std::string id = EcuIdHex(*init);
    LogEcuId(s, id);

    // read_mem() :219-318. Every page must be a complete, checksummed E0
    // frame carrying exactly one page; legacy appended any E0 reply.
    const MemoryRegion region = plan.TransferRegion();
    const auto pages = static_cast<int>(region.length / kPage);
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (int page = 0; page < pages; ++page)
    {
        const std::uint32_t address = region.start + static_cast<std::uint32_t>(page) * kPage;
        auto block = ExchangeExpect(s, ComposeBe(0xa0_b, 0x00_b, U24(address), bytes::Byte(kPage - 1)),
                                    kExtraLongTimeout, 0xe0, std::format("block read at 0x{:06X}", address));
        if (!block.has_value())
        {
            return std::unexpected(block.error());
        }
        if (block->size() != 4 + 1 + kPage + 1)
        {
            return Fail(ErrorKind::kBadResponse,
                        std::format("block read at 0x{:06X} returned {} bytes", address, block->size()));
        }
        rom.insert(rom.end(), block->begin() + 5, block->begin() + 5 + kPage);
        s.events.Progress(page + 1, pages);
        if (Status paced = s.clock.Sleep(kPagePacing, s.cancellation); !paced.has_value())
        {
            return std::unexpected(paced.error());
        }
    }
    return FlashExecutionResult{.operation = FlashOperation::kRead, .read_bytes = std::move(rom), .rom_id = id + "_"};
}

bool IsExactReply(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::ByteView payload)
{
    return ssm_protocol::HasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == payload.size() &&
           std::ranges::equal(frame.subspan(4, payload.size()), payload);
}

// write_mem() :448-516. read() returns whole frames, so legacy's byte
// accumulation has no counterpart: an empty read continues the poll, and any
// frame returned must be exactly `expected`. Exhausting the rounds fails;
// legacy's second poll fell through to programming.
Status PollFor(Session& s, bytes::ByteView expected, int rounds, std::string_view what)
{
    for (int round = 0; round < rounds; ++round)
    {
        auto response = Receive(s, kMediumTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!IsExactReply(**response, s.wire, expected))
            {
                return Fail(ErrorKind::kBadResponse, std::format("{} failed: {}", what, bytes::ToHex(**response)));
            }
            return {};
        }
        if (Status slept = s.clock.Sleep(kErasePollSleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return Fail(ErrorKind::kTimeout, std::format("no {} response after {} polls", what, rounds));
}

// write_mem() :346-432. Returns once the ECU is in flash mode at 19200 baud.
Status EnterFlashMode(Session& s, std::uint32_t rom_size)
{
    if (Status baud = s.transport.SetBaud(kWriteBaud); !baud.has_value())
    {
        return baud;
    }
    s.events.Log(LogLevel::kInfo, "Checking if OBK is running");
    if (Status sent = Send(s, ComposeBe(0xaf_b)); !sent.has_value())
    {
        return sent;
    }
    auto probe = Receive(s, kTimeout);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && HasSid(**probe, s.wire, 0xef))
    {
        return {};
    }
    s.events.Log(LogLevel::kInfo, "OBK not running, requesting flash mode");

    if (Status baud = s.transport.SetBaud(kColdBaud); !baud.has_value())
    {
        return baud;
    }
    auto init = ExpectSsmInit(s);
    if (!init.has_value())
    {
        return std::unexpected(init.error());
    }
    LogEcuId(s, EcuIdHex(*init));
    // send_sid_af_enter_flash_mode() :690-714. Gated: legacy logged a
    // rejection here and went on to raise VPP and erase.
    s.events.Log(LogLevel::kInfo, "Sending request to change to flash mode");
    auto entered = ExchangeExpect(
        s, ComposeBe(0xaf_b, 0x11_b, bytes::ByteView(*init).subspan(kEcuIdOffset, kEcuIdLength), U24(rom_size)),
        kTimeout, 0xef, "enter flash mode");
    if (!entered.has_value())
    {
        return std::unexpected(entered.error());
    }
    s.events.Log(LogLevel::kDebug, "Changing baudrate to 19200");
    return s.transport.SetBaud(kWriteBaud);
}

Status WriteRom(Session& s, const FlashPlan& plan)
{
    const bytes::Bytes& image = plan.ImageOrEmpty();
    const std::uint32_t rom_size = plan.TransferRegion().length;
    if (Status entered = EnterFlashMode(s, rom_size); !entered.has_value())
    {
        return entered;
    }

    // write_mem() :440-441. The operator confirmed external VPP before the
    // run when the adapter cannot supply it.
    if (Status cancelled = CancelledIfRequested(s.cancellation, "before programming voltage"); !cancelled.has_value())
    {
        return cancelled;
    }
    s.events.Log(LogLevel::kDebug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.EnableProgrammingVoltageLine(); !raised.has_value())
    {
        return raised;
    }

    // send_sid_af_erase_memory_block() :716-731 sends without reading.
    s.events.Log(LogLevel::kInfo, "Sending request to erase flash");
    if (Status sent = Send(s, ComposeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status started = PollFor(s, ComposeBe(0xef_b, 0x42_b), kEraseStartRounds, "flash erase start");
        !started.has_value())
    {
        return started;
    }
    s.events.Log(LogLevel::kInfo, "Flash erase in progress, please wait...");
    if (Status erased = PollFor(s, ComposeBe(0xef_b, 0x52_b), kEraseDoneRounds, "flash erase"); !erased.has_value())
    {
        return erased;
    }
    s.events.Log(LogLevel::kInfo, "Flash erased!");
    // write_mem() :517: one more read, whose result legacy discarded.
    auto trailing = Receive(s, kMediumTimeout);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        s.events.Log(LogLevel::kDebug, std::format("Discarded after erase: {}", bytes::ToHex(**trailing)));
    }

    // write_mem() :535-625.
    const auto blocks = static_cast<int>(rom_size / kPage);
    const bytes::Bytes done = ComposeBe(0xef_b, 0x52_b);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kPage;
        const bool last = block == blocks - 1;
        bytes::Bytes data(image.begin() + address, image.begin() + address + kPage);
        for (bytes::Byte& value : data)
        {
            value ^= kBlockXor;
        }
        if (Status sent = Send(s, ComposeBe(0xaf_b, last ? 0x69_b : 0x61_b, U24(address), bytes::ByteView(data)));
            !sent.has_value())
        {
            return sent;
        }
        auto response = Receive(s, kExtraLongTimeout);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (!response->has_value())
        {
            if (!last)
            {
                return Fail(ErrorKind::kTimeout, std::format("no response to block write at 0x{:06X}", address));
            }
            // Legacy never read a reply to AF 69 (:564); its shape is unknown.
            s.events.Log(LogLevel::kWarning, "No reply to the final block; treating the write as complete");
        }
        else if (!IsExactReply(**response, s.wire, done))
        {
            return Fail(ErrorKind::kBadResponse,
                        std::format("block write at 0x{:06X} failed: {}", address, bytes::ToHex(**response)));
        }
        s.events.Progress(block + 1, blocks);
    }
    s.events.Log(LogLevel::kInfo, "ROM written to flash.");
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rKlineExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rKlinePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :50-57.
    return NonIso14230KlineConfigFrom(std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.FamilyPlan()));
}

Status SubaruUnisiaJecsM32rKlineExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                   const ICancellationToken&) const
{
    // execute() :50. Clears a header left enabled by an earlier session.
    return transport.SetAddIso14230Header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rKlineExecutor::Execute(const FlashPlan& plan,
                                                                        IKlineFlashTransport& transport, IClock& clock,
                                                                        const ICancellationToken& cancellation,
                                                                        IEventSink& events)
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rKline); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rKlinePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events, std::get<SubaruUnisiaJecsM32rKlinePlan>(plan.FamilyPlan())};
    if (plan.Operation() == FlashOperation::kRead)
    {
        return ReadRom(session, plan);
    }

    const Status written = WriteRom(session, plan);
    // execute() :71-72: the LEC lines drop after write_mem() on every outcome.
    events.Log(LogLevel::kDebug, "Removing programming voltage +12v from Line End Check 1");
    const Status dropped = transport.DisableLecLines();
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
