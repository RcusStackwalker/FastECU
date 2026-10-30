#include "src/backend/service_functions/relearn_session.h"

#include <chrono>
#include <format>
#include <utility>

namespace fastecu::service_functions
{
namespace
{
using namespace std::chrono_literals;

constexpr int kWriteAttempts = 6;
constexpr int kPollIterations = 200;
constexpr std::chrono::milliseconds kReadTimeout = 200ms; // serial_read_short_timeout
constexpr bytes::Byte kWriteAck = 0xf8;
constexpr bytes::Byte kReadAck = 0xe8;

const bytes::Bytes kStepOne{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfc, 0x01};
const bytes::Bytes kStepTwo{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfd, 0x09};
// Legacy intends this; see the header for why it cannot build it.
const bytes::Bytes kPoll{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x01, 0xfd};

// Sends `frame` up to six times, accepting `expected` at index 4. Returns the
// last frame seen. A bad or absent response is tolerated by the caller, which
// is what legacy does with its commented-out returns.
Result<bytes::Bytes> ExchangeTolerantly(ISsmTransport& transport, const ICancellationToken& cancellation,
                                        bytes::ByteView frame, bytes::Byte expected)
{
    bytes::Bytes last;
    for (int attempt = 0; attempt < kWriteAttempts; ++attempt)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled before a TCU relearn write");
        }
        if (const auto sent = transport.Write(frame); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }

        const auto received = transport.Read(kReadTimeout, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        if (!received->has_value())
        {
            continue;
        }

        last = **received;
        if (last.size() > 4 && last[4] == expected)
        {
            return last;
        }
    }
    return last;
}

// A wrong response is logged, not fatal, as in legacy.
Status WriteStep(ISsmTransport& transport, const ICancellationToken& cancellation, IEventSink& events, int step,
                 bytes::ByteView frame)
{
    events.Log(LogLevel::kInfo, std::format("Initialising TCU relearn, step {}...", step));
    const auto response = ExchangeTolerantly(transport, cancellation, frame, kWriteAck);
    if (!response.has_value())
    {
        return std::unexpected(response.error());
    }
    if (response->size() <= 4 || (*response)[4] != kWriteAck)
    {
        events.Log(LogLevel::kError, std::format("Wrong response from TCU on relearn step {}; continuing", step));
    }
    return {};
}

// Which status value means "relearn complete" is not recoverable from the
// legacy source, so no terminal condition is invented: the bound is the
// legacy's 200 and the frame is surfaced for the bench.
Result<RelearnOutcome> PollStatus(ISsmTransport& transport, const ICancellationToken& cancellation, IEventSink& events)
{
    events.Log(LogLevel::kInfo, "Tracking relearn status...");
    RelearnOutcome outcome;
    for (int poll = 0; poll < kPollIterations; ++poll)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled while tracking relearn status");
        }
        if (const auto sent = transport.Write(kPoll); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }
        const auto received = transport.Read(kReadTimeout, cancellation);
        if (!received.has_value())
        {
            return std::unexpected(received.error());
        }
        ++outcome.polls_performed;
        events.Progress(outcome.polls_performed, kPollIterations);
        if (!received->has_value())
        {
            continue;
        }
        outcome.last_status_frame = **received;
        if (outcome.last_status_frame.size() > 4 && outcome.last_status_frame[4] != kReadAck)
        {
            // Logged, not fatal.
            events.Log(LogLevel::kError, "Unexpected relearn status response; continuing");
        }
    }
    return outcome;
}

} // namespace

RelearnSession::RelearnSession(std::string protocol) : protocol_(std::move(protocol))
{
}

Result<SsmTransportConfig> RelearnSession::TransportSetup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return Fail(ErrorKind::kUnsupported, std::format("not a Subaru Denso SH705x TCU protocol: {}", protocol_));
    }
    // configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
    return SsmTransportConfig{};
}

void RelearnSession::Submit(GateResponse response)
{
    gate_outstanding_ = false;
    declined_ = response == GateResponse::kDecline;
}

ServiceFunctionStep RelearnSession::Resume(ISsmTransport& transport, IClock&, const ICancellationToken& cancellation,
                                           IEventSink& events)
{
    if (gate_outstanding_)
    {
        return FailedStep{Error{ErrorKind::kInternal, "resume() called with an operator gate outstanding"}};
    }
    if (declined_)
    {
        return FailedStep{Error{ErrorKind::kCancelled, "operator declined a relearn gate"}};
    }
    if (cancellation.Cancelled())
    {
        return FailedStep{Error{ErrorKind::kCancelled, "cancelled during TCU relearn"}};
    }

    switch (stage_)
    {
    case Stage::kAwaitStaticSetupGate:
        // Engine at temperature, car off the ground, engine off, ignition on,
        // stick in P.
        stage_ = Stage::kWriteSteps;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::kRelearnStaticSetup};

    case Stage::kWriteSteps:
        for (const auto& [step, frame] : {std::pair{1, &kStepOne}, std::pair{2, &kStepTwo}})
        {
            if (auto written = WriteStep(transport, cancellation, events, step, *frame); !written.has_value())
            {
                return FailedStep{written.error()};
            }
        }
        // The gate that cannot be pre-collected.
        stage_ = Stage::kPoll;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::kRelearnEngineRunning};

    case Stage::kPoll:
    {
        auto outcome = PollStatus(transport, cancellation, events);
        if (!outcome.has_value())
        {
            return FailedStep{outcome.error()};
        }
        stage_ = Stage::kDone;
        return CompletedStep{*outcome};
    }

    case Stage::kDone:
        break;
    }

    return FailedStep{Error{ErrorKind::kInternal, "relearn resumed after completion"}};
}

} // namespace fastecu::service_functions
