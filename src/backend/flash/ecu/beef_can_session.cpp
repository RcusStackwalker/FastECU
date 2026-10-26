#include "src/backend/flash/ecu/beef_can_session.h"

#include <array>
#include <chrono>
#include <optional>
#include <format>
#include <string>

#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"

namespace fastecu::flash
{
namespace
{

constexpr std::chrono::milliseconds kNonfatalReadTimeout{2000};
constexpr std::chrono::milliseconds kNonfatalSendDelay{50};

// request_kernel_id(): the three trailing zero bytes are outside the declared
// BEEF body but are present on the legacy wire, so they stay literal.
constexpr bytes::Byte kKernelId = 0x01;
constexpr std::array<bytes::Byte, 8> kKernelIdRequest{0xBE, 0xEF, 0x00, 0x01, kKernelId, 0x00, 0x00, 0x00};
constexpr std::chrono::milliseconds kKernelIdTimeout{800};
constexpr std::chrono::milliseconds kKernelIdDelay{200};

Result<std::string> ParseKernelId(bytes::ByteView pdu)
{
    return ParseBeef(pdu).and_then(
        [](const BeefMessage& parsed) -> Result<std::string>
        {
            if (parsed.opcode != static_cast<bytes::Byte>(kKernelId | 0x40U))
            {
                return Fail(ErrorKind::kBadResponse, "unexpected kernel ID response opcode");
            }
            return std::string(parsed.payload.begin(), parsed.payload.end());
        });
}

} // namespace

struct BeefCanSession::View
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    ICanFlashTransport& transport;
    uds::IUdsChannel& channel;
};

BeefCanSession::BeefCanSession(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                               IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile)
    : transport_(transport), clock_(clock), cancellation_(cancellation), events_(events), channel_(channel),
      profile_(profile)
{
}

BeefCanSession::View BeefCanSession::MakeView() const
{
    return View{cancellation_, events_, clock_, transport_, channel_};
}

void BeefCanSession::LogWrongReply(const Error& failure) const
{
    LogError(MakeView(), std::format("Wrong response from {}: {}", profile_.device, failure.detail));
}

void BeefCanSession::LogWrongPdu(bytes::ByteView pdu) const
{
    const std::string rendered = profile_.pdu_format == BeefPduFormat::kHex ? bytes::ToHex(pdu) : uds::Describe(pdu);
    LogError(MakeView(), std::format("Wrong response from {}: {}", profile_.device, rendered));
}

void BeefCanSession::LogFailure(const Error& failure) const
{
    if (failure.kind == ErrorKind::kTimeout)
    {
        LogError(MakeView(), "No valid response from ECU");
    }
    else if (failure.kind == ErrorKind::kBadResponse)
    {
        LogWrongReply(failure);
    }
}

Result<std::optional<bytes::Bytes>> BeefCanSession::NonfatalQuery(bytes::ByteView pdu)
{
    View view = MakeView();
    Result<std::optional<bytes::Bytes>> received =
        ChannelRequestOptional(view, pdu, kNonfatalReadTimeout, kNonfatalSendDelay);
    if (!received.has_value())
    {
        const Error& failure = received.error();
        if (failure.kind == ErrorKind::kCancelled || failure.kind == ErrorKind::kDisconnected)
        {
            return std::unexpected(failure);
        }
        if (failure.kind == ErrorKind::kTimeout)
        {
            LogError(view, "No valid response from ECU");
        }
        else
        {
            LogWrongReply(failure);
        }
        return std::optional<bytes::Bytes>{};
    }
    if (!received->has_value())
    {
        LogError(view, "No valid response from ECU");
    }
    return received;
}

Result<AcceptedSessionModes> BeefCanSession::RequestBootloaderSessions()
{
    LogInfo(MakeView(), "Requesting session mode");
    return RequestSession(0x03).and_then(
        [this](bool mode_03)
        {
            return RequestSession(0x43).transform([mode_03](bool mode_43)
                                                  { return AcceptedSessionModes{mode_03, mode_43}; });
        });
}

Result<bool> BeefCanSession::RequestSession(bytes::Byte subfunction)
{
    Result<std::optional<bytes::Bytes>> reply =
        NonfatalQuery(bytes::Bytes{uds::kSidDiagnosticSessionControl, subfunction});
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return false;
    }
    const bytes::Bytes& pdu = **reply;
    if (pdu.size() >= 2 && pdu[0] == 0x50 && pdu[1] == subfunction)
    {
        return true;
    }
    LogWrongPdu(pdu);
    return false;
}

Result<std::optional<std::string>> BeefCanSession::RunningKernelId()
{
    View view = MakeView();
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(view, kKernelIdRequest, kKernelIdTimeout, kKernelIdDelay);
    if (!reply.has_value() && reply.error().kind == ErrorKind::kDisconnected)
    {
        return std::unexpected(reply.error());
    }
    if (reply.has_value() && !reply->has_value())
    {
        return std::nullopt;
    }
    return CancelledIfRequested(view, "kernel ID probe cancelled")
        .transform(
            [&]() -> std::optional<std::string>
            {
                Result<std::string> id =
                    reply.and_then([](const std::optional<bytes::Bytes>& pdu) { return ParseKernelId(*pdu); });
                if (!id.has_value())
                {
                    LogFailure(id.error());
                    return std::nullopt;
                }
                return *id;
            });
}

Status BeefCanSession::StartedKernelId()
{
    View view = MakeView();
    Result<std::optional<bytes::Bytes>> reply =
        ChannelRequestOptional(view, kKernelIdRequest, kKernelIdTimeout, kKernelIdDelay);
    if (!reply.has_value() && reply.error().kind != ErrorKind::kTimeout)
    {
        LogFailure(reply.error());
        return std::unexpected(reply.error());
    }
    if (!reply.has_value() || !reply->has_value())
    {
        LogError(view, "No valid response from ECU");
        return Fail(ErrorKind::kTimeout, "kernel did not answer after upload");
    }
    Result<std::string> id = ParseKernelId(**reply);
    if (!id.has_value())
    {
        LogFailure(id.error());
        return std::unexpected(id.error());
    }
    LogInfo(view, std::format("Kernel ID: {}", *id));
    return {};
}

Status BeefCanSession::RequestEcuId(bool read_operation, std::optional<std::string>& rom_id)
{
    LogInfo(MakeView(), "Requesting ECU ID");
    Result<std::optional<bytes::Bytes>> ecu_reply = NonfatalQuery(bytes::Bytes{0xAA});
    if (!ecu_reply.has_value())
    {
        return std::unexpected(ecu_reply.error());
    }
    if (!ecu_reply->has_value())
    {
        return {};
    }
    const bytes::Bytes& pdu = **ecu_reply;
    if (pdu.size() < 9 || pdu[0] != 0xEA)
    {
        LogWrongPdu(pdu);
        return {};
    }
    const std::string ecu_id = std::format("{:02X}{:02X}{:02X}{:02X}{:02X}", pdu[4], pdu[5], pdu[6], pdu[7], pdu[8]);
    LogInfo(MakeView(), std::format("ECU ID: {}", ecu_id));
    if (read_operation)
    {
        rom_id = ecu_id + "_";
    }
    return {};
}

Status BeefCanSession::RequestCvn()
{
    LogInfo(MakeView(), "Requesting CVN");
    Result<std::optional<bytes::Bytes>> cvn_reply = NonfatalQuery(bytes::Bytes{uds::kSidVehicleInfoRequest, 0x06});
    if (!cvn_reply.has_value())
    {
        return std::unexpected(cvn_reply.error());
    }
    if (!cvn_reply->has_value())
    {
        return {};
    }
    const bytes::Bytes& pdu = **cvn_reply;
    if (pdu.size() >= 3 && pdu[0] == 0x49 && pdu[1] == 0x06)
    {
        LogInfo(MakeView(), std::format("CVN: {}", UppercaseHexCompact(bytes::ByteView(pdu).subspan(3))));
    }
    else
    {
        LogWrongPdu(pdu);
    }
    return {};
}

Result<std::optional<std::string>> BeefCanSession::IdentityString(bytes::ByteView request, bytes::Byte first,
                                                                  bytes::Byte second, std::string_view label)
{
    Result<std::optional<bytes::Bytes>> reply = NonfatalQuery(request);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    if (!reply->has_value())
    {
        return std::optional<std::string>{};
    }
    const bytes::Bytes& pdu = **reply;
    if (pdu.size() < 3 || pdu[0] != first || pdu[1] != second)
    {
        LogWrongPdu(pdu);
        return std::optional<std::string>{};
    }
    const std::string value(pdu.begin() + 3, pdu.end());
    LogInfo(MakeView(), std::format("{}: {}", label, value));
    return std::optional<std::string>{value};
}

} // namespace fastecu::flash
