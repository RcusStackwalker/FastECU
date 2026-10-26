#pragma once

#include <optional>
#include <string>
#include <string_view>

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

// The bootloader sessions an ECU accepted, which select the programming
// session modes requested next.
struct AcceptedSessionModes
{
    bool mode_03;
    bool mode_43;
};

// The Denso BEEF CAN dialect's shared conversation with the ECU: tolerant
// cold-path queries and identity over one channel. The family keeps connect
// and kernel start; BeefCanKernel owns the warm transfer, read and write flows.
// Non-owning: the caller keeps every collaborator and the profile alive.
class BeefCanSession
{
  public:
    BeefCanSession(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                   IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile);

    // A Tolerant reply: a wrong or absent reply is logged and stepped over;
    // only cancellation or a disconnect aborts.
    Result<std::optional<bytes::Bytes>> NonfatalQuery(bytes::ByteView pdu);

    // Logs why a strict exchange failed: a timeout is "No valid response", a
    // bad reply names the device and the error detail, anything else is
    // silent (the caller returns it).
    void LogFailure(const Error& failure) const;

    // True when the ECU accepts the diagnostic session `subfunction`.
    Result<bool> RequestSession(bytes::Byte subfunction);
    // Requests the bootloader sessions 0x03 and then 0x43, each as a nonfatal
    // query, and reports which the ECU accepted.
    Result<AcceptedSessionModes> RequestBootloaderSessions();

    // The ID a running kernel answers the kernel-ID request with. Silence, a
    // transport error, or a short, malformed or wrong-id frame means no kernel;
    // only cancellation (checked once a reply or a failure is in) and
    // disconnection abort.
    Result<std::optional<std::string>> RunningKernelId();
    // Requires the just-started kernel's ID, as a Required reply: silence is
    // kTimeout "kernel did not answer after upload", a wrong reply kBadResponse.
    Status StartedKernelId();

    // Logs the ECU ID; a read names its image after it.
    Status RequestEcuId(bool read_operation, std::optional<std::string>& rom_id);
    Status RequestCvn();

    // The text after the three-byte header of a reply that opens `first`,
    // `second`, logged under `label`. Empty when the reply is wrong or absent.
    Result<std::optional<std::string>> IdentityString(bytes::ByteView request, bytes::Byte first, bytes::Byte second,
                                                      std::string_view label);

  private:
    // The shape denso_beef_can_common.h's templates read their context from.
    struct View;
    View MakeView() const;
    void LogWrongReply(const Error& failure) const;
    void LogWrongPdu(bytes::ByteView pdu) const;

    ICanFlashTransport& transport_;
    IClock& clock_;
    const ICancellationToken& cancellation_;
    IEventSink& events_;
    uds::IUdsChannel& channel_;
    const DensoBeefCanProfile& profile_;
};

} // namespace fastecu::flash
