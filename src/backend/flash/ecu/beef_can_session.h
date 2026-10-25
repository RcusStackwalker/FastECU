#pragma once

#include <optional>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/uds/iuds_channel.h"

namespace fastecu::flash
{

// The Denso BEEF CAN dialect's shared conversation with the ECU: tolerant
// queries, identity, CRC compare, flash init and write over one channel. The
// family keeps connect, kernel upload and everything its profile cannot say.
// Non-owning: the caller keeps every collaborator and the profile alive.
class BeefCanSession
{
  public:
    BeefCanSession(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile);

    // A Tolerant reply: a wrong or absent reply is logged and stepped over;
    // only cancellation or a disconnect aborts.
    Result<std::optional<bytes::Bytes>> NonfatalQuery(bytes::ByteView pdu);

  private:
    // The shape denso_beef_can_common.h's templates read their context from.
    struct View;
    View MakeView() const;
    void LogWrongReply(const Error& failure);

    ICanFlashTransport& transport_;
    IClock& clock_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    uds::IUdsChannel& channel_;
    const DensoBeefCanProfile& profile_;
};

} // namespace fastecu::flash
