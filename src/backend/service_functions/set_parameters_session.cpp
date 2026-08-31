#include "src/backend/service_functions/set_parameters_session.h"

#include <utility>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::service_functions
{
namespace
{

constexpr int kReadTimeoutMs = 500; // receive_timeout, legacy header :59
constexpr int kAttempts = 6;
constexpr bytes::Byte kTesterId = 0xf0; // legacy :151
constexpr bytes::Byte kTargetId = 0x18; // legacy :152
constexpr bytes::Byte kPositiveResponse = 0xf8;

bytes::Bytes frameFor(const TcuParameterWrite& write)
{
    // legacy :210-215 -- SID 0xB8, 24-bit address, value, framed exactly once.
    bytes::Bytes payload{0xb8};
    bytes::appendU24Be(payload, write.address);
    payload.push_back(write.value);
    return SsmProtocol::addHeader(payload, kTesterId, kTargetId);
}

} // namespace

SetParametersSession::SetParametersSession(std::string protocol, TcuParameterValues values)
    : protocol_(std::move(protocol)), values_(values)
{
}

Result<SsmTransportConfig> SetParametersSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, "not a Subaru Denso SH705x TCU protocol: " + protocol_);
    }
    // legacy :141-152 -- "CAN 0xb8 command is disabled, so switch to K-Line comms".
    return SsmTransportConfig{
        .framing = SsmTransportConfig::Framing::Kline14230,
        .bitrate_or_baud = 4800,
        .request_id = 0,
        .response_id = 0,
        .tester_id = kTesterId,
        .target_id = kTargetId,
        .add_iso14230_header = false,
    };
}

void SetParametersSession::submit(GateResponse)
{
    misused_ = true;
}

ServiceFunctionStep SetParametersSession::resume(ISsmTransport& transport, IClock&,
                                                 const ICancellationToken& cancellation, IEventSink& events)
{
    if (misused_)
    {
        return FailedStep{Error{ErrorKind::Internal, "set parameters has no operator gate to answer"}};
    }

    events.log(LogLevel::Info, "Setting TCU parameters...");

    const auto writes = tcu_parameter_writes(values_);
    int written_count = 0;

    for (const auto& write : writes)
    {
        bool acknowledged = false;
        bool received_frame = false;
        bytes::Bytes last_reply;

        for (int attempt = 0; attempt < kAttempts; ++attempt)
        {
            if (cancellation.cancelled())
            {
                return FailedStep{Error{ErrorKind::Cancelled, "cancelled while setting TCU parameters"}};
            }

            const bytes::Bytes frame = frameFor(write);
            if (const auto sent = transport.write(frame); !sent.has_value())
            {
                return FailedStep{sent.error()};
            }

            const auto received = transport.read(kReadTimeoutMs, cancellation);
            if (!received.has_value())
            {
                return FailedStep{received.error()};
            }
            if (!received->has_value())
            {
                continue;
            }

            // legacy :219-236 -- this check is NOT commented out, unlike relearn's.
            received_frame = true;
            last_reply = **received;
            if (last_reply.size() > 4 && last_reply[4] == kPositiveResponse)
            {
                acknowledged = true;
                break;
            }
        }

        if (!acknowledged)
        {
            if (!received_frame)
            {
                return FailedStep{Error{ErrorKind::Timeout, "no response to TCU parameter write"}};
            }
            return FailedStep{
                Error{ErrorKind::BadResponse, "TCU rejected a parameter write: " + bytes::toHex(last_reply)}};
        }

        ++written_count;
        events.progress(written_count, static_cast<int>(writes.size()));
    }

    return CompletedStep{SetParametersOutcome{.frames_written = written_count}};
}

} // namespace fastecu::service_functions
