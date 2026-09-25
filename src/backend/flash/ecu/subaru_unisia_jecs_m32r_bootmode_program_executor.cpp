#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_program_executor.h"

#include <algorithm>
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
using bytes::composeBe;
using bytes::u24;
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

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

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

bool is_status_reply(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    return SsmProtocol::hasValidFrame(frame, wire.tester_id, wire.target_id) && frame[3] == 2 && frame[4] == 0xef;
}

bool is_status(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire, bytes::Byte status)
{
    return is_status_reply(frame, wire) && frame[5] == status;
}

// write_mem() :346-353 documents these as error codes; success replies are
// EF 42 and EF 52, so they are read as the status byte after EF.
std::string_view status_meaning(bytes::Byte status)
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

std::string describe(bytes::ByteView frame, const SubaruUnisiaJecsM32rBootModeProgramPlan& wire)
{
    if (is_status_reply(frame, wire))
    {
        return std::format("{} (status {:02X}: {})", bytes::toHex(frame), frame[5], status_meaning(frame[5]));
    }
    return bytes::toHex(frame);
}

// write_mem() :393-463. read() returns whole frames: an empty read continues
// the poll, and any frame must be EF <status>. Legacy's first poll fell
// through on one to six bytes and its second never failed.
Status poll_for(Session& s, bytes::Byte status, std::chrono::milliseconds sleep, std::string_view what)
{
    for (int round = 0; round < kEraseRounds; ++round)
    {
        auto response = receive(s, kPollRead);
        if (!response.has_value())
        {
            return std::unexpected(response.error());
        }
        if (response->has_value())
        {
            if (!is_status(**response, s.wire, status))
            {
                return fail(ErrorKind::BadResponse, std::format("{} failed: {}", what, describe(**response, s.wire)));
            }
            return {};
        }
        if (Status slept = s.clock.sleep(sleep, s.cancellation); !slept.has_value())
        {
            return slept;
        }
    }
    return fail(ErrorKind::Timeout, std::format("no {} response after {} polls", what, kEraseRounds));
}

Status program(Session& s, const FlashPlan& plan)
{
    if (Status cancelled = cancelled_if_requested(s.cancellation, "before programming voltage"); !cancelled.has_value())
    {
        return cancelled;
    }
    // write_mem() :366: VPP stays, MOD1 drops. The operator removed MOD1
    // before this attempt started (the workflow's RemoveMod1 prompt).
    s.events.log(LogLevel::Debug, "Set programming voltage +12v to Line End Check 1");
    if (Status raised = s.transport.enable_programming_voltage_line(); !raised.has_value())
    {
        return raised;
    }

    s.events.log(LogLevel::Info, "Requesting flash erase, please wait...");
    if (Status sent = send(s, composeBe(0xaf_b, 0x31_b)); !sent.has_value())
    {
        return sent;
    }
    if (Status settled = s.clock.sleep(kEraseSettle, s.cancellation); !settled.has_value())
    {
        return settled;
    }
    if (Status started = poll_for(s, 0x42, kEraseStartSleep, "flash erase start"); !started.has_value())
    {
        return started;
    }
    s.events.log(LogLevel::Info, "Flash erase in progress, please wait...");
    if (Status erased = poll_for(s, 0x52, kEraseDoneSleep, "flash erase"); !erased.has_value())
    {
        return erased;
    }
    s.events.log(LogLevel::Info, "Flash erased!");
    if (Status settled = s.clock.sleep(kPostErase, s.cancellation); !settled.has_value())
    {
        return settled;
    }

    // write_mem() :467-578. Data goes as-is; unlike 6c-3 there is no XOR.
    const bytes::Bytes& image = *plan.image();
    const auto blocks = static_cast<int>(image.size() / kBlock);
    for (int block = 0; block < blocks; ++block)
    {
        const std::uint32_t address = static_cast<std::uint32_t>(block) * kBlock;
        const bool last = block == blocks - 1;
        const bytes::ByteView data = bytes::ByteView(image).subspan(address, kBlock);
        if (Status sent = send(s, composeBe(0xaf_b, last ? 0x69_b : 0x61_b, u24(address), data)); !sent.has_value())
        {
            return sent;
        }
        auto response = receive(s, kBlockTimeout);
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
            // Legacy never read a reply to AF 69 (:517); its shape is unknown.
            s.events.log(LogLevel::Warning, "No reply to the final block; treating the write as complete");
        }
        else if (!is_status(**response, s.wire, 0x52))
        {
            return fail(ErrorKind::BadResponse,
                        std::format("block write at 0x{:06X} failed: {}", address, describe(**response, s.wire)));
        }
        s.events.progress(block + 1, blocks);
        if (!last)
        {
            if (Status paced = s.clock.sleep(kBlockPacing, s.cancellation); !paced.has_value())
            {
                return paced;
            }
        }
    }
    s.events.log(LogLevel::Info, "ROM written to flash.");
    // write_mem() :581.
    s.events.log(LogLevel::Info, "Please remove VPP voltage, power cycle ECU and request SSM Init to confirm.");
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeProgramExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // write_mem() :361-364: no parity, 19200 baud.
    return non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.family_plan()));
}

Status SubaruUnisiaJecsM32rBootModeProgramExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                               const ICancellationToken&) const
{
    // write_mem() :361.
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult>
SubaruUnisiaJecsM32rBootModeProgramExecutor::execute(const FlashPlan& plan, IKlineFlashTransport& transport,
                                                     IClock& clock, const ICancellationToken& cancellation,
                                                     IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeProgram); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    Session session{transport, clock, cancellation, events,
                    std::get<SubaruUnisiaJecsM32rBootModeProgramPlan>(plan.family_plan())};
    const Status written = program(session, plan);
    // execute() :85, :89: legacy dropped the lines after write_mem().
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
