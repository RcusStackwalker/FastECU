#include "src/backend/service_functions/relearn_session.h"

#include <format>
#include <utility>

namespace fastecu::service_functions
{
namespace
{

constexpr int kWriteAttempts = 6;    // legacy :702
constexpr int kPollIterations = 200; // legacy :748
constexpr int kReadTimeoutMs = 200;  // serial_read_short_timeout, legacy header :62
constexpr bytes::Byte kWriteAck = 0xf8;
constexpr bytes::Byte kReadAck = 0xe8;

// legacy :655-663.
const bytes::Bytes kStepOne{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfc, 0x01};
// legacy :699-700.
const bytes::Bytes kStepTwo{0x00, 0x00, 0x07, 0xe1, 0xb8, 0x00, 0x01, 0xfd, 0x09};
// legacy :740-747 intends this; see the header for why it cannot build it.
const bytes::Bytes kPoll{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00, 0x00, 0x01, 0xfc, 0x00, 0x01, 0xfd};

// Sends `frame` up to six times, accepting `expected` at index 4. Returns the
// last frame seen. A bad or absent response is tolerated by the caller, which
// is what legacy :688/:694/:722/:733 do with their commented-out returns.
Result<bytes::Bytes> exchangeTolerantly(ISsmTransport& transport, const ICancellationToken& cancellation,
                                        bytes::ByteView frame, bytes::Byte expected)
{
    bytes::Bytes last;
    for (int attempt = 0; attempt < kWriteAttempts; ++attempt)
    {
        if (const auto sent = transport.write(frame); !sent.has_value())
        {
            return std::unexpected(sent.error());
        }

        const auto received = transport.read(kReadTimeoutMs, cancellation);
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

} // namespace

RelearnSession::RelearnSession(std::string protocol) : protocol_(std::move(protocol))
{
}

Result<SsmTransportConfig> RelearnSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, std::format("not a Subaru Denso SH705x TCU protocol: {}", protocol_));
    }
    // legacy :70 -- configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
    return SsmTransportConfig{};
}

void RelearnSession::submit(GateResponse response)
{
    gate_outstanding_ = false;
    declined_ = response == GateResponse::Decline;
}

ServiceFunctionStep RelearnSession::resume(ISsmTransport& transport, IClock&, const ICancellationToken& cancellation,
                                           IEventSink& events)
{
    if (gate_outstanding_)
    {
        return FailedStep{Error{ErrorKind::Internal, "resume() called with an operator gate outstanding"}};
    }
    if (declined_)
    {
        return FailedStep{Error{ErrorKind::Cancelled, "operator declined a relearn gate"}};
    }
    if (cancellation.cancelled())
    {
        return FailedStep{Error{ErrorKind::Cancelled, "cancelled during TCU relearn"}};
    }

    switch (stage_)
    {
    case Stage::AwaitStaticSetupGate:
        // legacy :648 -- engine at temperature, car off the ground, engine off,
        // ignition on, stick in P.
        stage_ = Stage::WriteSteps;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::RelearnStaticSetup};

    case Stage::WriteSteps:
    {
        events.log(LogLevel::Info, "Initialising TCU relearn, step 1...");
        const auto first = exchangeTolerantly(transport, cancellation, kStepOne, kWriteAck);
        if (!first.has_value())
        {
            return FailedStep{first.error()};
        }
        if (first->size() <= 4 || (*first)[4] != kWriteAck)
        {
            // legacy :688/:694 -- logged, not fatal.
            events.log(LogLevel::Error, "Wrong response from TCU on relearn step 1; continuing");
        }

        events.log(LogLevel::Info, "Initialising TCU relearn, step 2...");
        const auto second = exchangeTolerantly(transport, cancellation, kStepTwo, kWriteAck);
        if (!second.has_value())
        {
            return FailedStep{second.error()};
        }
        if (second->size() <= 4 || (*second)[4] != kWriteAck)
        {
            // legacy :722/:733 -- logged, not fatal.
            events.log(LogLevel::Error, "Wrong response from TCU on relearn step 2; continuing");
        }

        // legacy :735 -- the gate that cannot be pre-collected.
        stage_ = Stage::Poll;
        gate_outstanding_ = true;
        return GateStep{OperatorGateId::RelearnEngineRunning};
    }

    case Stage::Poll:
    {
        events.log(LogLevel::Info, "Tracking relearn status...");
        RelearnOutcome outcome;
        for (int poll = 0; poll < kPollIterations; ++poll)
        {
            if (cancellation.cancelled())
            {
                return FailedStep{Error{ErrorKind::Cancelled, "cancelled while tracking relearn status"}};
            }

            if (const auto sent = transport.write(kPoll); !sent.has_value())
            {
                return FailedStep{sent.error()};
            }

            const auto received = transport.read(kReadTimeoutMs, cancellation);
            if (!received.has_value())
            {
                return FailedStep{received.error()};
            }

            ++outcome.polls_performed;
            events.progress(outcome.polls_performed, kPollIterations);
            if (received->has_value())
            {
                outcome.last_status_frame = **received;
                if (outcome.last_status_frame.size() > 4 && outcome.last_status_frame[4] != kReadAck)
                {
                    // legacy :771/:777 -- logged, not fatal.
                    events.log(LogLevel::Error, "Unexpected relearn status response; continuing");
                }
            }
        }

        // Which status value means "relearn complete" is not recoverable from
        // the legacy source, so no terminal condition is invented: the bound
        // is the legacy's 200 and the frame is surfaced for the bench.
        stage_ = Stage::Done;
        return CompletedStep{outcome};
    }

    case Stage::Done:
        break;
    }

    return FailedStep{Error{ErrorKind::Internal, "relearn resumed after completion"}};
}

} // namespace fastecu::service_functions
