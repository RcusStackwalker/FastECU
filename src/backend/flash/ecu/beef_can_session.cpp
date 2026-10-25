#include "src/backend/flash/ecu/beef_can_session.h"

#include <chrono>
#include <format>
#include <string>

#include "src/backend/flash/ecu/denso_beef_can_common.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"

namespace fastecu::flash
{
namespace
{

constexpr std::chrono::milliseconds kNonfatalReadTimeout{2000};
constexpr std::chrono::milliseconds kNonfatalSendDelay{50};

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

void BeefCanSession::LogWrongReply(const Error& failure)
{
    std::string suffix;
    switch (profile_.wrong_reply)
    {
    case BeefWrongReply::kBare:
        break;
    case BeefWrongReply::kNotValidAnswer:
        suffix = ": Not a valid answer";
        break;
    case BeefWrongReply::kErrorDetail:
        suffix = std::format(": {}", failure.detail);
        break;
    }
    LogError(MakeView(), std::format("Wrong response from {}{}", profile_.device, suffix));
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

} // namespace fastecu::flash
