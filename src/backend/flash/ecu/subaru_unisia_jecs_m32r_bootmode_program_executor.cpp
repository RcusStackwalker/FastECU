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

Status CancelledIfRequested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::format("cancelled {}", where));
    }
    return {};
}

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

Status Program(Session& s, const FlashPlan& plan)
{
    if (Status cancelled = CancelledIfRequested(s.cancellation, "before programming voltage"); !cancelled.has_value())
    {
        return cancelled;
    }
    // write_mem() :366: VPP stays, MOD1 drops. The operator removed MOD1
    // before this attempt started (the workflow's RemoveMod1 prompt).
    s.events.Log(LogLevel::kDebug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.EnableProgrammingVoltageLine(); !raised.has_value())
    {
        return raised;
    }

    s.events.Log(LogLevel::kInfo, "Requesting flash erase, please wait...");
    if (Status sent = Send(s, ComposeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status settled = s.clock.Sleep(kEraseSettle, s.cancellation); !settled.has_value())
    {
        return settled;
    }
    if (Status started = PollFor(s, 0x42, kEraseStartSleep, "flash erase start"); !started.has_value())
    {
        return started;
    }
    s.events.Log(LogLevel::kInfo, "Flash erase in progress, please wait...");
    if (Status erased = PollFor(s, 0x52, kEraseDoneSleep, "flash erase"); !erased.has_value())
    {
        return erased;
    }
    s.events.Log(LogLevel::kInfo, "Flash erased!");
    if (Status settled = s.clock.Sleep(kPostErase, s.cancellation); !settled.has_value())
    {
        return settled;
    }

    // write_mem() :467-578. Data goes as-is; unlike 6c-3 there is no XOR.
    const bytes::Bytes& image = plan.ImageOrEmpty();
    const auto blocks = static_cast<int>(image.size() / kBlock);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kBlock;
        const bool last = block == blocks - 1;
        const bytes::ByteView data = bytes::ByteView(image).subspan(address, kBlock);
        if (Status sent = Send(s, ComposeBe(0xaf_b, last ? 0x69_b : 0x61_b, U24(address), data)); !sent.has_value())
        {
            return sent;
        }
        auto response = Receive(s, kBlockTimeout);
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
            // Legacy never read a reply to AF 69 (:517); its shape is unknown.
            s.events.Log(LogLevel::kWarning, "No reply to the final block; treating the write as complete");
        }
        else if (!IsStatus(**response, s.wire, 0x52))
        {
            return Fail(ErrorKind::kBadResponse,
                        std::format("block write at 0x{:06X} failed: {}", address, Describe(**response, s.wire)));
        }
        s.events.Progress(block + 1, blocks);
        if (!last)
        {
            if (Status paced = s.clock.Sleep(kBlockPacing, s.cancellation); !paced.has_value())
            {
                return paced;
            }
        }
    }
    s.events.Log(LogLevel::kInfo, "ROM written to flash.");
    // write_mem() :581.
    s.events.Log(LogLevel::kInfo, "Please remove VPP voltage, power cycle ECU and request SSM Init to confirm.");
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeProgramExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rBootmodePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // write_mem() :361-364: no parity, 19200 baud.
    return NonIso14230KlineConfigFrom(std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.FamilyPlan()));
}

Status SubaruUnisiaJecsM32rBootModeProgramExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                             const ICancellationToken&) const
{
    // write_mem() :361.
    if (Status reset = transport.ResetConnection(); !reset.has_value())
    {
        return reset;
    }
    return transport.SetAddIso14230Header(false);
}

Result<FlashExecutionResult>
SubaruUnisiaJecsM32rBootModeProgramExecutor::Execute(const FlashPlan& plan, IKlineFlashTransport& transport,
                                                     IClock& clock, const ICancellationToken& cancellation,
                                                     IEventSink& events)
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rBootmodePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events,
                    std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.FamilyPlan())};
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
