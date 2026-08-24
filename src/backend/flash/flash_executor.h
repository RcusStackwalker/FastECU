#pragma once
#include <concepts>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

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

struct KlineConfig
{
    int baud;
    bool iso14230;
    std::uint8_t tester_id;
    std::uint8_t target_id;
};

struct Iso15765Config
{
    int bitrate;
    std::uint32_t request_id;
    std::uint32_t response_id;
    bool extended_id;
};

class IKlineFlashTransport;
class ICanFlashTransport;

// The caller owns transport lifetime. An executor never calls configure(),
// open(), or close() on the transport it is given: it receives a transport
// already configured per transport_setup() and open, uses it, and returns.
// Mid-session operations that belong to a protocol sequence -- setBaud(),
// set_add_iso14230_header(), the LEC line calls -- are not lifecycle and stay
// in the executor. See docs/adr/0015-caller-owns-flash-transport-lifetime.md.
class IKlineFlashExecutor
{
  public:
    using TransportType = IKlineFlashTransport;
    using ConfigType = KlineConfig;

    virtual ~IKlineFlashExecutor() = default;

    // Pure: validates `plan` and returns the configuration this executor
    // requires. Performs no I/O, so an invalid plan is rejected before the
    // caller touches hardware.
    virtual Result<KlineConfig> transport_setup(const FlashPlan& plan) const = 0;

    // Family-specific cancellation checkpoint after configure() and before
    // open(). Most executors historically had no checkpoint in that interval.
    virtual Status before_transport_open(const ICancellationToken&) const
    {
        return {};
    }

    virtual Result<FlashExecutionResult> execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events) = 0;
};

// CAN sibling of IKlineFlashExecutor; the same contract applies.
class ICanFlashExecutor
{
  public:
    using TransportType = ICanFlashTransport;
    using ConfigType = Iso15765Config;

    virtual ~ICanFlashExecutor() = default;

    virtual Result<Iso15765Config> transport_setup(const FlashPlan& plan) const = 0;

    virtual Status before_transport_open(const ICancellationToken&) const
    {
        return {};
    }

    virtual Result<FlashExecutionResult> execute(const FlashPlan& plan, ICanFlashTransport& transport, IClock& clock,
                                                 const ICancellationToken& cancellation, IEventSink& events) = 0;
};

// A constructed FlashPlan already has a transport kind and variant consistent
// with its family (validated in flash_validation.cpp). Once this succeeds,
// std::get<PlanT>(plan.family_plan()) cannot throw.
Status check_family(const FlashPlan& plan, FlashFamily expected_family);

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
    virtual Result<std::optional<bytes::Bytes>> read(int timeout_ms, const ICancellationToken&) = 0;
};

// An executor already bound to a transport it is known to accept. FlashWorker
// holds this instead of the two halves, so no caller can pair an executor with
// the wrong transport: bind_flash_attempt is the only way to construct one and
// its requires-clause rejects a mismatch at compile time.
class BoundFlashAttempt
{
  public:
    virtual ~BoundFlashAttempt() = default;
    virtual const FlashPlan& plan() const noexcept = 0;
    virtual Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation,
                                             IEventSink& events) = 0;
    virtual void request_unblock() noexcept = 0;
};

template <class Executor, class Transport> class BoundAttempt final : public BoundFlashAttempt
{
  public:
    BoundAttempt(FlashPlan plan, std::unique_ptr<Executor> executor, std::unique_ptr<Transport> transport)
        : plan_(std::move(plan)), executor_(std::move(executor)), transport_(std::move(transport))
    {
    }

    const FlashPlan& plan() const noexcept override
    {
        return plan_;
    }

    Result<FlashExecutionResult> run(IClock& clock, const ICancellationToken& cancellation, IEventSink& events) override
    {
        // Pure: validates the plan and derives config, touching no hardware, so
        // a bad plan is rejected before the adapter is configured or opened.
        Result<typename Executor::ConfigType> setup = executor_->transport_setup(plan_);
        if (!setup.has_value())
        {
            return std::unexpected(setup.error());
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "cancelled before configure");
        }
        if (const Status configured = transport_->configure(*setup); !configured.has_value())
        {
            return std::unexpected(configured.error());
        }
        if (const Status checkpoint = executor_->before_transport_open(cancellation); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        if (const Status opened = transport_->open(); !opened.has_value())
        {
            return std::unexpected(opened.error());
        }

        Result<FlashExecutionResult> outcome = executor_->execute(plan_, *transport_, clock, cancellation, events);

        // Exactly once on every exit path past open(). Main error wins over a
        // close error; a close-only error is returned. This was step 5c's
        // EEPROM-family rule; here it is the universal one.
        const Status closed = transport_->close();
        if (!outcome.has_value())
        {
            if (!closed.has_value())
            {
                events.log(LogLevel::Warning, "close failed after execution error");
            }
            return outcome;
        }
        if (!closed.has_value())
        {
            return std::unexpected(closed.error());
        }
        return outcome;
    }

    void request_unblock() noexcept override
    {
        transport_->request_unblock();
    }

  private:
    FlashPlan plan_;
    std::unique_ptr<Executor> executor_;
    std::unique_ptr<Transport> transport_;
};

template <class Executor, class Transport>
    requires std::derived_from<Transport, typename Executor::TransportType>
std::unique_ptr<BoundFlashAttempt> bind_flash_attempt(FlashPlan plan, std::unique_ptr<Executor> executor,
                                                      std::unique_ptr<Transport> transport)
{
    return std::make_unique<BoundAttempt<Executor, Transport>>(std::move(plan), std::move(executor),
                                                               std::move(transport));
}

} // namespace fastecu::flash
