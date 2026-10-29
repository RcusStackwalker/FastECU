#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

// Internals of the Denso BEEF dialect CAN executors: SubaruDensoSh7058CanExecutor,
// SubaruDensoSh7058CanDieselExecutor and SubaruTcuDensoSh705xCanExecutor.
//
// Transitional cold-path utilities and the outer operation runner. Session
// owns tolerant identity queries; the executors retain connect/security and
// kernel start/handoff. ParseBeef is also used by cold kernel-ID probing.
// BeefCanKernel owns all warm transfer/read/write behavior, including the
// concrete BEEF exchange and CRC/flash sequences.
//
// The executor suites do NOT read these helpers back: each carries wire
// expectations transcribed independently from its legacy oracle, so a wrong
// change here fails those suites rather than passing silently. Keep it that
// way.
namespace fastecu::flash
{

// The two-byte frame marker every BEEF-protocol request and reply opens with.
// Byte-identical across all three consumers.
inline constexpr std::uint32_t kKernelStartComm = 0xBEEF;

// A parsed BEEF reply: its opcode and the payload the declared length covers.
struct BeefMessage
{
    bytes::Byte opcode;
    bytes::ByteView payload;
};

inline Result<BeefMessage> ParseBeef(bytes::ByteView pdu)
{
    if (pdu.size() < 5)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response envelope");
    }
    if (bytes::ReadU16Be(pdu) != kKernelStartComm)
    {
        return Fail(ErrorKind::kBadResponse, "wrong BEEF response marker");
    }
    const std::uint16_t declared = bytes::ReadU16Be(pdu, 2);
    // The legacy serial reader can return a complete ISO payload with
    // transport padding or a coalesced trailing frame.  Its checks only use
    // the declared BEEF body, so retain that tolerance while bounding every
    // indexed field.  The view exposed to callers is limited to the declared
    // opcode-plus-payload body.
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > pdu.size())
    {
        return Fail(ErrorKind::kBadResponse, "invalid BEEF response length");
    }
    return BeefMessage{.opcode = pdu[4], .payload = pdu.subspan(5, declared - 1)};
}

// Upper-case hex of `data` with no separators.
inline std::string UppercaseHexCompact(bytes::ByteView data)
{
    std::string rendered;
    for (const bytes::Byte value : data)
    {
        std::format_to(std::back_inserter(rendered), "{:02X}", value);
    }
    return rendered;
}

// Any executor context carrying a cancellation token.
template <typename C>
concept WithCancellation = requires(const C& ctx) {
    { ctx.cancellation } -> std::convertible_to<const ICancellationToken&>;
};

template <WithCancellation C> Status CancelledIfRequested(const C& ctx, std::string_view detail)
{
    if (ctx.cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::string(detail));
    }
    return {};
}

// The request half of every BEEF exchange: cancellation check, send on the
// context's channel, then an optional post-send settle delay. Templated because
// each family's channel member has its own type.
template <typename C> Status SendChannelRequest(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds delay)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "cancelled before CAN request"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    if (delay > std::chrono::milliseconds{0})
    {
        return ctx.clock.Sleep(delay, ctx.cancellation);
    }
    return {};
}

// Sends `pdu` and returns whatever arrives within `timeout`.
template <typename C>
Result<std::optional<bytes::Bytes>>
ChannelRequestOptional(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    if (const Status sent = SendChannelRequest(ctx, pdu, delay); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    return ctx.channel.Receive(timeout, ctx.cancellation);
}

// ChannelRequestOptional where silence is a timeout error.
template <typename C>
Result<bytes::Bytes> ChannelRequest(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                                    std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    Result<std::optional<bytes::Bytes>> received = ChannelRequestOptional(ctx, pdu, timeout, delay);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no CAN response within the read timeout");
    }
    return std::move(**received);
}

// Requests a programming session in mode 0x02 for an accepted session 0x03
// and mode 0x42 for an accepted session 0x43, and accepts a reply echoing
// either mode.
template <typename C>
Status EnterBeefProgrammingSession(C& ctx, bool session_03_accepted, bool session_43_accepted,
                                   const uds::ExchangePolicy& policy)
{
    LogInfo(ctx, "Set session mode");
    bytes::Bytes request{uds::kSidDiagnosticSessionControl};
    if (session_03_accepted)
    {
        request.push_back(0x02);
    }
    if (session_43_accepted)
    {
        request.push_back(0x42);
    }
    Result<bytes::Bytes> reply = ctx.uds.Request(request, policy, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.empty() || (payload[0] != 0x02 && payload[0] != 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(ctx, "Succesfully set to programming session");
    return {};
}

// The petrol and diesel security unlock: a strict seed request (27 01, whose
// reply carries at least five payload bytes opening 01), the family's
// `seed_key(ctx, seed)` of the four seed bytes, then the strict key (27 02,
// whose reply opens 02). A security-access reply without a payload is refused,
// and every failure is logged through the session.
template <typename C, typename SeedKey>
Status UnlockBeefSecurity(C& ctx, SeedKey seed_key, const uds::ExchangePolicy& policy)
{
    const auto strict = [&](const bytes::Bytes& pdu)
    {
        Result<bytes::Bytes> reply = ctx.uds.Request(pdu, policy, ctx.cancellation);
        if (reply.has_value() && reply->size() <= 1)
        {
            reply = std::unexpected(Error{ErrorKind::kBadResponse, "security access reply is too short"});
        }
        if (!reply.has_value())
        {
            ctx.session.LogFailure(reply.error());
        }
        return reply;
    };
    LogInfo(ctx, "Requesting seed");
    return strict(bytes::Bytes{uds::kSidSecurityAccess, uds::kSecurityAccessRequestSeed})
        .and_then(
            [&](const bytes::Bytes& reply) -> Result<bytes::Bytes>
            {
                const bytes::ByteView payload = uds::Payload(reply);
                if (payload.size() < 5 || payload[0] != uds::kSecurityAccessRequestSeed)
                {
                    LogError(ctx, "Wrong response from ECU: Not a valid answer");
                    return Fail(ErrorKind::kBadResponse, "invalid seed response");
                }
                LogInfo(ctx, "Seed request ok");
                return seed_key(ctx, payload.subspan(1, 4));
            })
        .and_then(
            [&](const bytes::Bytes& key)
            {
                LogInfo(ctx, "Sending seed key");
                return strict(bytes::ComposeBe(uds::kSidSecurityAccess, uds::kSecurityAccessSendKey, key));
            })
        .and_then(
            [&](const bytes::Bytes& reply) -> Status
            {
                if (uds::Payload(reply)[0] != uds::kSecurityAccessSendKey)
                {
                    const Error mismatch{ErrorKind::kBadResponse, "unexpected response during seed key"};
                    ctx.session.LogFailure(mismatch);
                    return std::unexpected(mismatch);
                }
                LogInfo(ctx, "Seed key ok");
                return {};
            });
}

// The petrol and diesel connect tail: asks the session which bootloader
// sessions the ECU accepts, unlocks with the family's `unlock(ctx)`, then
// enters the programming session those sessions select.
template <typename C, typename Unlock>
Status ConnectBeefProgrammingSession(C& ctx, Unlock unlock, const uds::ExchangePolicy& policy)
{
    auto sessions = ctx.session.RequestBootloaderSessions();
    if (!sessions.has_value())
    {
        return std::unexpected(sessions.error());
    }
    if (Status unlocked = unlock(ctx); !unlocked.has_value())
    {
        return unlocked;
    }
    return EnterBeefProgrammingSession(ctx, sessions->mode_03, sessions->mode_43, policy);
}

// The Denso BEEF CAN operation: a "Kernel" phase that asks whether a kernel
// already answers and, when none does, connects to `unit`'s bootloader and
// uploads the plan's kernel; then a "Read" of the transfer region, or a
// "Compare" over the erase regions that the family's write continues before a
// closing "Complete" phase.
//   probe(ctx) -> Result<std::optional<std::string>>, the running kernel's ID
//   connect(ctx, read_operation, rom_id&) -> Status
//   upload(ctx, const KernelImage&) -> Status
//   read(ctx, const FlashPlan&, PhaseReporter&) -> Result<bytes::Bytes>
//   write(ctx, const FlashPlan&, PhaseSequence&, PhaseReporter& compare) -> Status
template <typename C, typename Probe, typename Connect, typename Upload, typename Read, typename Write>
Result<FlashExecutionResult> RunBeefKernelOperation(const FlashPlan& plan, std::string_view unit, C& ctx, Probe probe,
                                                    Connect connect, Upload upload, Read read, Write write)
{
    const bool read_operation = plan.Operation() == FlashOperation::kRead;
    PhaseSequence phases(ctx.events, read_operation ? 2 : 4);
    PhaseReporter kernel_phase = phases.Start("Kernel", 1);
    std::optional<std::string> rom_id;
    LogInfo(ctx, std::format("Connecting to {} CAN bootloader, please wait...", unit));
    LogInfo(ctx, "Checking if kernel is already running...");
    LogInfo(ctx, "Requesting kernel ID");
    Result<std::optional<std::string>> kernel_id = probe(ctx);
    if (!kernel_id.has_value())
    {
        return std::unexpected(kernel_id.error());
    }
    if (kernel_id->has_value())
    {
        LogInfo(ctx, std::format("Kernel ID: {}", **kernel_id));
    }
    else
    {
        LogError(ctx, "No valid response from ECU");
        LogInfo(ctx, "No response from kernel, initialising ECU...");
        if (const Status connected = connect(ctx, read_operation, rom_id); !connected.has_value())
        {
            return std::unexpected(connected.error());
        }
        if (plan.Kernel().has_value())
        {
            ctx.events.Notice("Preparing, please wait...");
            LogInfo(ctx, std::format("Initializing {} CAN kernel upload, please wait...", unit));
            if (const Status uploaded = upload(ctx, *plan.Kernel()); !uploaded.has_value())
            {
                return std::unexpected(uploaded.error());
            }
        }
    }
    kernel_phase.Complete();

    if (read_operation)
    {
        ctx.events.Notice("Reading ROM, please wait...");
        LogInfo(ctx, std::format("Reading ROM from {} using CAN", unit));
        PhaseReporter read_phase = phases.Start("Read", static_cast<int>(plan.TransferRegion().length));
        Result<bytes::Bytes> rom = read(ctx, plan, read_phase);
        if (!rom.has_value())
        {
            return std::unexpected(rom.error());
        }
        read_phase.Complete();
        return FlashExecutionResult{
            .operation = FlashOperation::kRead, .read_bytes = std::move(*rom), .rom_id = std::move(rom_id)};
    }

    ctx.events.Notice("Writing ROM, please wait...");
    LogInfo(ctx, std::format("Writing ROM to {} using CAN", unit));
    PhaseReporter compare_phase = phases.Start("Compare", static_cast<int>(plan.EraseRegions().size()));
    if (const Status written = write(ctx, plan, phases, compare_phase); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    PhaseReporter complete = phases.Start("Complete", 1);
    complete.Complete();
    return FlashExecutionResult{.operation = plan.Operation()};
}

} // namespace fastecu::flash
