#pragma once
#include <cstdint>
#include <optional>

#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_plan.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/clock.h"
#include "src/backend/ports/event_sink.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/ikline_transport.h"

namespace fastecu::flash
{

struct FlashExecutionResult
{
    FlashOperation operation;
    std::optional<bytes::Bytes> read_bytes; // present for successful Read
    std::optional<std::string> rom_id;
};

// Lifetime/unblock capability only; it deliberately has no universal I/O API.
class IFlashTransport
{
  public:
    virtual ~IFlashTransport() = default;
    virtual void request_unblock() noexcept = 0;
};

class IFlashExecutor
{
  public:
    virtual ~IFlashExecutor() = default;
    virtual Result<FlashExecutionResult> execute(
        const FlashPlan& plan,
        IFlashTransport& transport,
        IClock& clock,
        const ICancellationToken& cancellation,
        IEventSink& events) = 0;
};

// Every concrete executor calls this first and returns its result verbatim
// on failure -- zero I/O happens before a family/transport mismatch is
// caught.
Status check_family_transport_match(const FlashPlan& plan, FlashFamily expected_family,
                                    TransportKind expected_transport);

struct KlineConfig
{
    int baud;
    bool iso14230;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

// Adds only configure/open/close/request_unblock to the already Result-based,
// cancellation-aware mutdma::IKlineTransport merged in step 5b (PR #78,
// commit 8ac6ba2) -- there is no second incompatible byte-stream interface.
class IKlineFlashTransport : public IFlashTransport, public mutdma::IKlineTransport
{
  public:
    virtual Status configure(const KlineConfig&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;

    // Hardware control-line operations used by bootloaders that require an
    // explicit LEC reset/pulse sequence before accepting K-Line traffic.
    // They are deliberately semantic rather than exposing the desktop
    // adapter's RTS/DTR integer states to portable executors.
    virtual Status disable_lec_lines() = 0;
    virtual Status pulse_lec_2_line(int timeout_ms) = 0;
    virtual Status enable_programming_voltage_line() = 0;

    // Some Unix J2534/OpenPort2 drivers need a quiet period after the raw
    // kernel upload write before the first response read. Portable
    // executors consume only this semantic capability, never adapter types.
    virtual bool requires_post_kernel_upload_delay() const = 0;

    // Controls the real serial driver's ISO14230 header auto-add behavior
    // (SerialPortActions::set_add_iso14230_header()), independently of
    // configure()'s connection-type flags and mid-session, not just at
    // configure() time. Some exchanges self-frame their own header via
    // SsmProtocol::addHeader() and need this OFF (false, the default) to
    // avoid double-framing; others hand the driver a raw, unframed request
    // and need this ON (true) so the driver adds the header itself. The
    // legacy, now-deleted EepromEcuSubaruDensoSH705xKlineOperation is exactly
    // this shape: connect()/upload_kernel() rely on the default false (lines
    // 199/329, commented out) while read_mem()'s raw SID_DUMP request turns
    // it on (line 477). See DensoSh705xEepromKlineExecutor::execute() for the
    // portable equivalent.
    virtual Status set_add_iso14230_header(bool add_header) = 0;
};

struct Iso15765Config
{
    int bitrate;
    std::uint32_t request_id;
    std::uint32_t response_id;
    bool extended_id;
};

// Distinct from cdbg::ICanTransport (raw CAN frames, used by CDBG logging):
// the proving CAN family configures SerialPortActions for ISO-15765 and
// exchanges framed byte messages, not raw single-frame CAN traffic.
class ICanFlashTransport : public IFlashTransport
{
  public:
    virtual ~ICanFlashTransport() = default;
    virtual Status configure(const Iso15765Config&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;
    virtual Status write(bytes::ByteView, const ICancellationToken&) = 0;
    virtual Result<std::optional<bytes::Bytes>> read(int timeout_ms,
                                                     const ICancellationToken&) = 0;
};

} // namespace fastecu::flash
