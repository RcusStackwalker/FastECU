#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"
#include "src/backend/flash/ecu/m32r_ssm_exchange.h"
#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

namespace fastecu::flash
{
namespace
{
using bytes::ComposeBe;
using bytes::U24;
using namespace bytes::literals;
using namespace std::chrono_literals;

// Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.{h,cpp}.
constexpr std::uint32_t kBlock = 0x80;   // write_mem() :473
constexpr auto kEraseSettle = 500ms;     // write_mem() :389
constexpr auto kPollRead = 10ms;         // write_mem() :400, :442
constexpr int kEraseRounds = 20;         // write_mem() :393, :435
constexpr auto kEraseStartSleep = 500ms; // write_mem() :421
constexpr auto kEraseDoneSleep = 1000ms; // write_mem() :462
constexpr auto kPostErase = 1000ms;      // write_mem() :465
constexpr auto kBlockTimeout = 3000ms;   // serial_read_extra_long_timeout, :518
constexpr auto kBlockPacing = 10ms;      // write_mem() :539

struct Session
{
    IKlineFlashTransport& transport;
    IClock& clock;
    const ICancellationToken& cancellation;
    IEventSink& events;
    const SubaruUnisiaJecsM32rBootModeProgramPlan& wire;
};

Status Send(Session& s, bytes::ByteView payload)
{
    return SendM32rSsmFrame(s.transport, s.cancellation, payload, s.wire.tester_id, s.wire.target_id);
}

Result<std::optional<bytes::Bytes>> Receive(Session& s, std::chrono::milliseconds timeout)
{
    return ReceiveM32rSsmOnce(s.transport, s.cancellation, timeout);
}

bool IsStatusReply(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    return ssm_protocol::HasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == 2 && frame[4] == 0xef;
}

bool IsStatus(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire, bytes::Byte status)
{
    return IsStatusReply(frame, wire) && frame[5] == status;
}

// write_mem() :346-353 documents these as error codes; success replies are
// EF 42 and EF 52, so they are read as the status byte after EF.
std::string_view StatusMeaning(bytes::Byte status)
{
    switch (status)
    {
    case 0x42:
        return "erase started";
    case 0x48:
        return "missing VPP voltage";
    case 0x52:
        return "done";
    case 0x5c:
        return "checksum error";
    case 0x72:
        return "address error";
    case 0x8a:
        return "FENTRY bit not set";
    default:
        return "unknown";
    }
}

std::string Describe(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    if (IsStatusReply(frame, wire))
    {
        return std::format("{} (status {:02X}: {})", bytes::ToHex(frame), frame[5], StatusMeaning(frame[5]));
    }
    return bytes::ToHex(frame);
}

// write_mem() :393-463. read() returns whole frames: an empty read continues
// the poll, and any frame must be EF <status>. Legacy's first poll fell
// through on one to six bytes and its second never failed.
Status PollFor(Session& s, bytes::Byte status, std::chrono::milliseconds sleep, std::string_view what)
{
    for (int round = 0; round < kEraseRounds; ++round)
    {
        auto response = Receive(s, kPollRead);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!IsStatus(**response, s.wire, status))
            {
                return Fail(ErrorKind::kBadResponse, std::format("{} failed: {}", what, Describe(**response, s.wire)));
            }
            return {};
        }
        if (Status slept = s.clock.Sleep(sleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return Fail(ErrorKind::kTimeout, std::format("no {} response after {} polls", what, kEraseRounds));
}

Status EraseFlash(Session& s)
{
    s.events.Log(LogLevel::kInfo, "Requesting flash erase, please wait...");
    return Send(s, ComposeBe(0xaf_b, 0x31_b))
        .and_then([&] { return s.clock.Sleep(kEraseSettle, s.cancellation); })
        .and_then([&] { return PollFor(s, 0x42, kEraseStartSleep, "flash erase start"); })
        .and_then(
            [&]
            {
                s.events.Log(LogLevel::kInfo, "Flash erase in progress, please wait...");
                return PollFor(s, 0x52, kEraseDoneSleep, "flash erase");
            })
        .and_then(
            [&]
            {
                s.events.Log(LogLevel::kInfo, "Flash erased!");
                return s.clock.Sleep(kPostErase, s.cancellation);
            });
}

// One block write; the final AF 69 block may go unanswered.
Status WriteBlock(Session& s, bytes::ByteView image, int block, int blocks)
{
    const std::uint32_t address = static_cast<std::uint32_t>(block) * kBlock;
    const bool last = block == blocks - 1;
    const bytes::ByteView data = image.subspan(address, kBlock);
    return Send(s, ComposeBe(0xaf_b, last ? 0x69_b : 0x61_b, U24{address}, data))
        .and_then([&] { return Receive(s, kBlockTimeout); })
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
                    // Legacy never read a reply to AF 69 (:517); its shape is unknown.
                    s.events.Log(LogLevel::kWarning, "No reply to the final block; treating the write as complete");
                }
                else if (!IsStatus(*response, s.wire, 0x52))
                {
                    return Fail(ErrorKind::kBadResponse, std::format("block write at 0x{:06X} failed: {}", address,
                                                                     Describe(*response, s.wire)));
                }
                s.events.Progress(block + 1, blocks);
                return last ? Status{} : s.clock.Sleep(kBlockPacing, s.cancellation);
            });
}

Status Program(Session& s, const FlashPlan& plan)
{
    // Plan admission is the cancellation checkpoint before the voltage.
    // write_mem() :366: VPP stays, MOD1 drops. The operator removed MOD1
    // before this attempt started (the workflow's RemoveMod1 prompt).
    s.events.Log(LogLevel::kDebug, "Set programming voltage +12v to Line End Check 1");
    if (Status erased = s.transport.EnableProgrammingVoltageLine().and_then([&] { return EraseFlash(s); });
        !erased.has_value())
    {
        return erased;
    }
    // write_mem() :467-578. Data goes as-is; unlike 6c-3 there is no XOR.
    const bytes::Bytes& image = plan.ImageOrEmpty();
    const auto blocks = static_cast<int>(image.size() / kBlock);
    for (int block = 0; block < blocks; ++block)
    {
        if (Status written = WriteBlock(s, image, block, blocks); !written.has_value())
        {
            return written;
        }
    }
    s.events.Log(LogLevel::kInfo, "ROM written to flash.");
    // write_mem() :581.
    s.events.Log(LogLevel::kInfo, "Please remove VPP voltage, power cycle ECU and request SSM Init to confirm.");
    return {};
}
} // namespace

Result<KlineConfig>
SubaruUnisiaJecsM32rBootModeProgramExecutor::Setup(const SubaruUnisiaJecsM32rBootModeProgramPlan& family) const
{
    // write_mem() :361-364: no parity, 19200 baud.
    return NonIso14230KlineConfigFrom(family);
}

Status SubaruUnisiaJecsM32rBootModeProgramExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                             const ICancellationToken&) const
{
    // write_mem() :361.
    return transport.ResetConnection().and_then([&] { return transport.SetAddIso14230Header(false); });
}

Result<FlashExecutionResult>
SubaruUnisiaJecsM32rBootModeProgramExecutor::Run(const SubaruUnisiaJecsM32rBootModeProgramPlan& family,
                                                 const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events)
{
    Session session{transport, clock, cancellation, events, family};
    const Status written = Program(session, plan);
    // execute() :85, :89: legacy dropped the lines after write_mem().
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
