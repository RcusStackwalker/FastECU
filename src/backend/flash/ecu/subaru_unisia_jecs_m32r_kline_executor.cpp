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
#include "src/backend/flash/ecu/m32r_ssm_exchange.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_kline_plan.h"
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

// Legacy header timeouts (flash_ecu_subaru_unisia_jecs_m32r_operation.h).
constexpr auto kTimeout = 2000ms;          // serial_read_timeout
constexpr auto kExtraLongTimeout = 3000ms; // serial_read_extra_long_timeout
constexpr auto kPagePacing = 1ms;
constexpr std::uint32_t kPage = 0x80;
constexpr int kColdBaud = 4800;
constexpr int kReadBaud = 38400;
// The ECU ID is the five bytes after the header, SID and three capability
// bytes of the BF reply.
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;
constexpr auto kMediumTimeout = 500ms; // serial_read_medium_timeout
constexpr int kWriteBaud = 19200;
constexpr auto kErasePollSleep = 500ms;
constexpr int kEraseStartRounds = 20;
constexpr int kEraseDoneRounds = 40;
constexpr bytes::Byte kBlockXor = 0x82;

struct Session
{
    IKlineFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
    const SubaruUnisiaJecsM32rKlinePlan& wire;
};

// Legacy write_serial_data_echo_check() of an SsmProtocol::addHeader frame.
Status Send(Session& s, bytes::ByteView payload)
{
    return SendM32rSsmFrame(s.transport, s.cancellation, payload, s.wire.tester_id, s.wire.target_id);
}

// One framed read; an empty optional means no frame arrived in time.
Result<std::optional<bytes::Bytes>> Receive(Session& s, std::chrono::milliseconds timeout)
{
    return ReceiveM32rSsmOnce(s.transport, s.cancellation, timeout);
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
    return Send(s, payload)
        .and_then([&] { return Receive(s, timeout); })
        .and_then(
            [&](std::optional<bytes::Bytes> response) -> Result<bytes::Bytes>
            {
                if (!response.has_value())
                {
                    return Fail(ErrorKind::kTimeout, std::format("no response to {}", what));
                }
                if (!HasSid(*response, s.wire, sid))
                {
                    return Fail(ErrorKind::kBadResponse,
                                std::format("unexpected response to {}: {}", what, bytes::ToHex(*response)));
                }
                return std::move(*response);
            });
}

// Gated: the reply must be FF and long
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

// Probe at 38400 in case the ECU is already in read mode.
// Returns its SSM init reply, or nothing when it must be woken cold.
Result<std::optional<bytes::Bytes>> ProbeReadMode(Session& s)
{
    return s.transport.SetBaud(kReadBaud)
        .and_then(
            [&]
            {
                s.events.Log(LogLevel::kInfo, "Checking if ECU in read mode");
                return Send(s, ComposeBe(0xbf_b));
            })
        .and_then([&] { return Receive(s, kTimeout); })
        .transform(
            [&](std::optional<bytes::Bytes> probe)
            {
                if (probe.has_value() && HasSid(*probe, s.wire, 0xff) && CarriesEcuId(*probe))
                {
                    return probe;
                }
                return std::optional<bytes::Bytes>{};
            });
}

// SSM init at the cold baud, then switch to 38400 and
// confirm. Returns the cold init reply.
Result<bytes::Bytes> InitReadMode(Session& s)
{
    Result<bytes::Bytes> cold = s.transport.SetBaud(kColdBaud).and_then([&] { return ExpectSsmInit(s); });
    if (!cold.has_value())
    {
        return cold;
    }
    return ExchangeExpect(s, ComposeBe(0xb8_b, 0x00_b, 0x00_b, 0x00_b, 0x75_b), kTimeout, 0xf8, "baud rate change")
        .and_then([&](const bytes::Bytes&) { return s.transport.SetBaud(kReadBaud); })
        .and_then(
            [&]
            {
                s.events.Log(LogLevel::kInfo, "Requesting ECU ID, checking if baudrate change was ok");
                return ExpectSsmInit(s);
            })
        .transform([&](const bytes::Bytes&) { return std::move(*cold); });
}

// Every page must be a complete, checksummed E0 frame
// carrying exactly one page; legacy appended any E0 reply.
Result<bytes::Bytes> ReadPages(Session& s, const MemoryRegion& region)
{
    const auto pages = static_cast<int>(region.length / kPage);
    bytes::Bytes rom;
    rom.reserve(region.length);
    for (int page = 0; page < pages; ++page)
    {
        const std::uint32_t address = region.start + static_cast<std::uint32_t>(page) * kPage;
        auto block = ExchangeExpect(s, ComposeBe(0xa0_b, 0x00_b, U24{address}, bytes::Byte(kPage - 1)),
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
    return rom;
}

Result<FlashExecutionResult> ReadRom(Session& s, const FlashPlan& plan)
{
    // A mismatch at 38400 is logged and the cold init follows, as legacy did.
    Result<std::optional<bytes::Bytes>> init = ProbeReadMode(s);
    if (!init.has_value())
    {
        return std::unexpected(init.error());
    }
    if (!init->has_value())
    {
        s.events.Log(LogLevel::kInfo, "Read mode not active, initialising ECU...");
        Result<bytes::Bytes> cold = InitReadMode(s);
        if (!cold.has_value())
        {
            return std::unexpected(cold.error());
        }
        *init = std::move(*cold);
    }
    const std::string id = EcuIdHex(**init);
    LogEcuId(s, id);
    Result<bytes::Bytes> rom = ReadPages(s, plan.TransferRegion());
    if (!rom.has_value())
    {
        return std::unexpected(rom.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::kRead, .read_bytes = std::move(*rom), .rom_id = id + "_"};
}

bool IsExactReply(bytes::ByteView frame, const SubaruUnisiaJecsM32rKlinePlan& wire, bytes::ByteView payload)
{
    return ssm_protocol::HasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == payload.size() &&
           std::ranges::equal(frame.subspan(4, payload.size()), payload);
}

// read() returns whole frames, so legacy's byte
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

// Probe at 19200 for an already-running OBK. Returns
// whatever frame arrived, or nothing.
Result<std::optional<bytes::Bytes>> ProbeObk(Session& s)
{
    return s.transport.SetBaud(kWriteBaud)
        .and_then(
            [&]
            {
                s.events.Log(LogLevel::kInfo, "Checking if OBK is running");
                return Send(s, ComposeBe(0xaf_b));
            })
        .and_then([&] { return Receive(s, kTimeout); });
}

// Returns once the ECU is in flash mode at 19200 baud.
Status EnterFlashMode(Session& s, std::uint32_t rom_size)
{
    Result<std::optional<bytes::Bytes>> probe = ProbeObk(s);
    if (!probe.has_value())
    {
        return std::unexpected(probe.error());
    }
    if (probe->has_value() && HasSid(**probe, s.wire, 0xef))
    {
        return {};
    }
    s.events.Log(LogLevel::kInfo, "OBK not running, requesting flash mode");

    return s.transport.SetBaud(kColdBaud)
        .and_then([&] { return ExpectSsmInit(s); })
        .and_then(
            [&](const bytes::Bytes& init)
            {
                LogEcuId(s, EcuIdHex(init));
                // Gated: legacy logged a
                // rejection here and went on to raise VPP and erase.
                s.events.Log(LogLevel::kInfo, "Sending request to change to flash mode");
                return ExchangeExpect(
                    s,
                    ComposeBe(0xaf_b, 0x11_b, bytes::ByteView(init).subspan(kEcuIdOffset, kEcuIdLength), U24{rom_size}),
                    kTimeout, 0xef, "enter flash mode");
            })
        .and_then(
            [&](const bytes::Bytes&)
            {
                s.events.Log(LogLevel::kDebug, "Changing baudrate to 19200");
                return s.transport.SetBaud(kWriteBaud);
            });
}

Status EraseFlash(Session& s)
{
    // Sends without reading.
    s.events.Log(LogLevel::kInfo, "Sending request to erase flash");
    return Send(s, ComposeBe(0xaf_b, 0x31_b))
        .and_then([&] { return PollFor(s, ComposeBe(0xef_b, 0x42_b), kEraseStartRounds, "flash erase start"); })
        .and_then(LogThen(s.events, "Flash erase in progress, please wait...",
                          [&] { return PollFor(s, ComposeBe(0xef_b, 0x52_b), kEraseDoneRounds, "flash erase"); }))
        .and_then(LogThen(s.events, "Flash erased!", [&] { return Receive(s, kMediumTimeout); }))
        .transform(
            [&](const std::optional<bytes::Bytes>& trailing)
            {
                if (trailing.has_value())
                {
                    s.events.Log(LogLevel::kDebug, std::format("Discarded after erase: {}", bytes::ToHex(*trailing)));
                }
            });
}

// One XOR-encoded block. The final AF 69 block may go
// unanswered.
Status WriteBlock(Session& s, const bytes::Bytes& image, int block, int blocks)
{
    const auto done = ComposeBe(0xef_b, 0x52_b);
    const std::uint32_t address = static_cast<std::uint32_t>(block) * kPage;
    const bool last = block == blocks - 1;
    bytes::Bytes data(image.begin() + address, image.begin() + address + kPage);
    for (bytes::Byte& value : data)
    {
        value ^= kBlockXor;
    }
    return Send(s, ComposeBe(0xaf_b, last ? 0x69_b : 0x61_b, U24{address}, bytes::ByteView(data)))
        .and_then([&] { return Receive(s, kExtraLongTimeout); })
        .and_then(
            [&](const std::optional<bytes::Bytes>& response) -> Status
            {
                if (!response.has_value())
                {
                    if (!last)
                    {
                        return Fail(ErrorKind::kTimeout,
                                    std::format("no response to block write at 0x{:06X}", address));
                    }
                    // Legacy never read a reply to AF 69; its shape is unknown.
                    s.events.Log(LogLevel::kWarning, "No reply to the final block; treating the write as complete");
                }
                else if (!IsExactReply(*response, s.wire, done))
                {
                    return Fail(ErrorKind::kBadResponse,
                                std::format("block write at 0x{:06X} failed: {}", address, bytes::ToHex(*response)));
                }
                s.events.Progress(block + 1, blocks);
                return {};
            });
}

Status WriteRom(Session& s, const FlashPlan& plan)
{
    const bytes::Bytes& image = plan.ImageOrEmpty();
    const std::uint32_t rom_size = plan.TransferRegion().length;
    if (Status erased =
            EnterFlashMode(s, rom_size)
                // The operator confirmed external VPP before the
                // run when the adapter cannot supply it.
                .and_then([&] { return CheckCancelled(s.cancellation, "cancelled before programming voltage"); })
                .and_then(
                    [&]
                    {
                        s.events.Log(LogLevel::kDebug, "Set programming voltage +12v to Line End Check 1");
                        return s.transport.EnableProgrammingVoltageLine();
                    })
                .and_then([&] { return EraseFlash(s); });
        !erased.has_value())
    {
        return erased;
    }
    const auto blocks = static_cast<int>(rom_size / kPage);
    for (int block = 0; block < blocks; ++block)
    {
        if (Status written = WriteBlock(s, image, block, blocks); !written.has_value())
        {
            return written;
        }
    }
    s.events.Log(LogLevel::kInfo, "ROM written to flash.");
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rKlineExecutor::Setup(const SubaruUnisiaJecsM32rKlinePlan& family) const
{
    return NonIso14230KlineConfigFrom(family);
}

Status SubaruUnisiaJecsM32rKlineExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                   const ICancellationToken&) const
{
    // Clears a header left enabled by an earlier session.
    return transport.SetAddIso14230Header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rKlineExecutor::Run(const SubaruUnisiaJecsM32rKlinePlan& family,
                                                                    const FlashPlan& plan,
                                                                    IKlineFlashTransport& transport, IClock& clock,
                                                                    const ICancellationToken& cancellation,
                                                                    IEventSink& events)
{
    Session session{transport, clock, cancellation, events, family};
    if (plan.Operation() == FlashOperation::kRead)
    {
        return ReadRom(session, plan);
    }

    const Status written = WriteRom(session, plan);
    // The LEC lines drop after write_mem() on every outcome.
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
