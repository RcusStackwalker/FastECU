#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/ecu/beef_compare_result.h"
#include "src/backend/flash/ecu/denso_beef_can_profile.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_executor.h"
#include "src/backend/flash/flash_plan.h"
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

    // Logs why a strict exchange failed: a timeout is "No valid response", a
    // bad reply names the device and the error detail, anything else is
    // silent (the caller returns it).
    void LogFailure(const Error& failure) const;

    // A BEEF request whose reply must carry `opcode | 0x40` and at least
    // `minimum_payload` payload bytes; returns that payload. Failures are
    // returned, not logged.
    Result<bytes::Bytes> Exchange(bytes::Byte opcode, bytes::ByteView payload, std::size_t minimum_payload,
                                  std::chrono::milliseconds timeout);

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

    // The CRC of `block` as the ECU's kernel computes it.
    Result<std::uint32_t> QueryCrc(const MemoryRegion& block);

    // Which erase regions of `plan` the ECU's CRC disagrees with `image` on.
    Result<CompareResult> CompareBlocks(const FlashPlan& plan, bytes::ByteView image, PhaseReporter *progress,
                                        bool after_reflash = false);

    // Reads the kernel's message and block sizes, then selects test or real
    // write mode.
    Status InitializeFlash(bool test_write);

    // Compares the ECU with the plan's image, then reflashes every differing
    // erase region and compares again.
    Status WriteMemory(const FlashPlan& plan, PhaseSequence& phases, PhaseReporter& compare_progress);

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
