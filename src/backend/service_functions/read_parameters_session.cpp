#include "src/backend/service_functions/read_parameters_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <utility>

namespace fastecu::service_functions
{
namespace
{

constexpr int kAttempts = 6;              // legacy :571
constexpr int kReadTimeoutMs = 200;       // serial_read_short_timeout, legacy header :62
constexpr std::size_t kMinFrameSize = 15; // bytes 5..14 are decoded; legacy guards > 10
constexpr bytes::Byte kPositiveResponse = 0xe8;

// legacy :540-570 -- ten addresses, in this order.
constexpr std::array<std::uint16_t, 10> kAddresses{0x16c, 0x16d, 0x16e, 0x16f, 0x170,
                                                   0x171, 0x1bc, 0x1bd, 0x1be, 0x1bf};

bytes::Bytes buildRequest()
{
    // legacy :534-539 -- 0x7E1 envelope, then SID 0xA8 and the "one time only"
    // response-mode byte.
    bytes::Bytes request{0x00, 0x00, 0x07, 0xe1, 0xa8, 0x00};
    for (const std::uint16_t address : kAddresses)
    {
        bytes::appendU24Be(request, address);
    }
    return request;
}

TcuParameterReadout decode(bytes::ByteView frame)
{
    // legacy :610-624 -- nine values across response bytes 5..14.
    return TcuParameterReadout{
        .input_clutch = frame[5],
        .high_low_reverse_clutch = frame[6],
        .direct_clutch = frame[7],
        .front_brake = frame[8],
        .awd_clutch_torque = bytes::readU16Be(frame, 9),
        .forward_brake = frame[11],
        .four_wheel_drive = frame[12],
        .line_pressure = frame[13],
        .temperature_basis = frame[14],
    };
}

} // namespace

ReadParametersSession::ReadParametersSession(std::string protocol) : protocol_(std::move(protocol))
{
}

Result<SsmTransportConfig> ReadParametersSession::transport_setup() const
{
    if (protocol_ != "sub_tcu_denso_sh7055_can" && protocol_ != "sub_tcu_denso_sh7058_can")
    {
        return fail(ErrorKind::Unsupported, std::format("not a Subaru Denso SH705x TCU protocol: {}", protocol_));
    }
    // legacy :70 -- configureIso15765Can(serial, "500000", 0x7E1, 0x7E9).
    return SsmTransportConfig{};
}

void ReadParametersSession::submit(GateResponse)
{
    misused_ = true;
}

ServiceFunctionStep ReadParametersSession::resume(ISsmTransport& transport, IClock&,
                                                  const ICancellationToken& cancellation, IEventSink& events)
{
    if (misused_)
    {
        return FailedStep{Error{ErrorKind::Internal, "read parameters has no operator gate to answer"}};
    }
    if (cancellation.cancelled())
    {
        return FailedStep{Error{ErrorKind::Cancelled, "cancelled before reading TCU parameters"}};
    }

    events.log(LogLevel::Info, "Reading TCU parameters...");

    const bytes::Bytes request = buildRequest();
    bytes::Bytes frame;

    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        if (const auto written = transport.write(request); !written.has_value())
        {
            return FailedStep{written.error()};
        }

        const auto received = transport.read(kReadTimeoutMs, cancellation);
        if (!received.has_value())
        {
            return FailedStep{received.error()};
        }
        if (!received->has_value())
        {
            continue; // deadline with no frame; legacy simply retries
        }

        frame = **received;
        // legacy accepts only 0xF8 here and then demands 0xE8 below, so it can
        // never succeed. 0xE8 is the positive response to the 0xA8 sent above.
        if (frame.size() > 4 && frame[4] == kPositiveResponse)
        {
            if (frame.size() < kMinFrameSize)
            {
                return FailedStep{Error{ErrorKind::BadResponse, "TCU parameter frame shorter than 15 bytes"}};
            }
            return CompletedStep{decode(frame)};
        }
    }

    if (frame.empty())
    {
        return FailedStep{Error{ErrorKind::Timeout, "no response to the TCU parameter read after 6 attempts"}};
    }
    return FailedStep{
        Error{ErrorKind::BadResponse, std::format("TCU rejected the parameter read: {}", bytes::toHex(frame))}};
}

} // namespace fastecu::service_functions
