#pragma once
#include <chrono>
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
#include "src/backend/protocol/ican_transport.h"
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

enum class KlineParity
{
    None,
    Even,
    Odd
};

struct KlineConfig
{
    int baud;
    bool iso14230;
    std::uint8_t tester_id;
    std::uint8_t target_id;
    KlineParity parity = KlineParity::None;
};

struct Iso15765Config
{
    int bitrate;
    std::uint32_t request_id;
    std::uint32_t response_id;
    bool extended_id;
};

struct RawCanConfig
{
    int bitrate;
    std::uint32_t transmit_id;
    std::uint32_t receive_id;
    bool extended_id;
};

struct MixedCanConfig
{
    Iso15765Config kernel;
    RawCanConfig bootloader;
};

template <class Plan>
concept Iso15765ConfigSource = requires(const Plan& plan) {
    { plan.bitrate } -> std::convertible_to<int>;
    { plan.request_id } -> std::convertible_to<std::uint32_t>;
    { plan.response_id } -> std::convertible_to<std::uint32_t>;
    { plan.extended_id } -> std::convertible_to<bool>;
};

template <Iso15765ConfigSource Plan> constexpr Iso15765Config iso15765_config_from(const Plan& plan) noexcept
{
    return Iso15765Config{
        .bitrate = plan.bitrate,
        .request_id = plan.request_id,
        .response_id = plan.response_id,
        .extended_id = plan.extended_id,
    };
}

template <class Plan>
concept KlineConfigSource = requires(const Plan& plan) {
    { plan.initial_baud } -> std::convertible_to<int>;
    { plan.tester_id } -> std::convertible_to<std::uint8_t>;
    { plan.target_id } -> std::convertible_to<std::uint8_t>;
};

template <KlineConfigSource Plan> constexpr KlineConfig non_iso14230_kline_config_from(const Plan& plan) noexcept
{
    return KlineConfig{
        .baud = plan.initial_baud,
        .iso14230 = false,
        .tester_id = plan.tester_id,
        .target_id = plan.target_id,
    };
}

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

    // Optional family-specific preparation before the caller configures the
    // adapter.  This is the narrow seam for protocols whose legacy startup
    // sequence performs a reset and a timed quiet period before setters.
    virtual Status before_transport_configure(IKlineFlashTransport&, IClock&, const ICancellationToken&) const
    {
        return {};
    }

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

    // Optional family-specific preparation before configure().  The default
    // keeps existing executors on the established lifecycle path.
    virtual Status before_transport_configure(ICanFlashTransport&, IClock&, const ICancellationToken&) const
    {
        return {};
    }

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
    // Raw serial calls bypass echo checking and framed reads. On J2534 the
    // two write paths coincide; on direct serial, write() drains local echo.
    virtual Result<std::size_t> write_raw(bytes::ByteView) = 0;
    virtual Result<OptionalBytes> read_raw(std::chrono::milliseconds, const ICancellationToken&) = 0;
    virtual Status configure(const KlineConfig&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;

    // Every K-Line transport must expose a real reset so protocol-owned
    // sequences cannot silently degrade into configure/open only. Mirrors
    // ICanFlashTransport::reset_connection(). Called only from an executor's
    // before_transport_configure(), never mid-session: the caller still owns
    // configure/open/close (ADR 0015).
    virtual Status reset_connection() = 0;

    // Hardware control-line operations used by bootloaders that require an
    // explicit LEC reset/pulse sequence before accepting K-Line traffic.
    // They are deliberately semantic rather than exposing the desktop
    // adapter's RTS/DTR integer states to portable executors.
    virtual Status disable_lec_lines() = 0;
    virtual Status pulse_lec_2_line(std::chrono::milliseconds timeout) = 0;
    virtual Status enable_programming_voltage_line() = 0;

    // Wave 7. Programming voltage on LEC1 and MOD1 on LEC2 together: the M32R
    // boot-mode entry state the Unisia Jecs bootmode kernel upload needs.
    virtual Status enable_boot_mode_lines() = 0;

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
    // Every CAN transport must expose a real reset so protocol-owned
    // sequences cannot silently degrade into configure/open only.
    virtual Status reset_connection() = 0;
    virtual Status configure(const Iso15765Config&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;
    // A protocol-owned in-session restart. This intentionally owns only
    // reset/reconfigure/reopen; BoundAttempt still owns initial setup and
    // final close.
    //
    // Post-condition on the last checkpoint below ("...after open"): if that
    // check reports Cancelled, reset/configure/open have already all
    // succeeded, so the port is left reset, reconfigured and OPEN -- a caller
    // cannot tell that outcome apart from a restart that never began. This is
    // safe for the current caller only because BoundAttempt::run() closes the
    // transport exactly once on every exit path past its own open() (see its
    // comment above the close() call), so a restart-time Cancelled here still
    // gets torn down at the end of the attempt regardless of this port state.
    virtual Status restart_iso15765(const Iso15765Config& config, const ICancellationToken& cancellation)
    {
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "ISO-15765 restart cancelled before reset");
        }
        if (const Status reset = reset_connection(); !reset)
        {
            return reset;
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "ISO-15765 restart cancelled after reset");
        }
        if (const Status configured = configure(config); !configured)
        {
            return configured;
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "ISO-15765 restart cancelled after configure");
        }
        if (const Status opened = open(); !opened)
        {
            return opened;
        }
        if (cancellation.cancelled())
        {
            return fail(ErrorKind::Cancelled, "ISO-15765 restart cancelled after open");
        }
        return {};
    }
    virtual Status write(bytes::ByteView, const ICancellationToken&) = 0;
    virtual Result<std::optional<bytes::Bytes>> read(std::chrono::milliseconds timeout, const ICancellationToken&) = 0;
};

// A transport for the DensoCAN family's two-phase flash sequence: a raw-CAN
// bootloader phase (enter_raw_bootloader_mode()/write_raw()/read_raw()) that
// uploads and hands off to a kernel, followed by a switch to ISO-15765
// (enter_iso15765_kernel_mode()/write_iso15765()/read_iso15765()) for the
// kernel's own framed traffic. configure()/open()/close() govern the whole
// session; the enter_*/write_*/read_* calls are what move between the two
// phases within it.
//
// cdbg::CanFrame appears here even though ICanFlashTransport's comment above
// explains that flash CAN is deliberately kept distinct from
// cdbg::ICanTransport: that separation is about the *framed* ISO-15765 side,
// which still exchanges plain bytes::Bytes here via write_iso15765()/
// read_iso15765(). The raw bootloader phase, in contrast, genuinely is
// single-frame id+payload CAN traffic -- the same shape cdbg::CanFrame
// already models -- so reusing it avoids inventing a second raw-frame type
// for one already-solved problem, without pulling cdbg::ICanTransport itself
// into the flash contract.
//
// No production consumer yet: it exists ahead of PR #341's DensoCAN family,
// which is what will actually construct and drive it.
class IMixedCanFlashTransport : public IFlashTransport
{
  public:
    virtual Status reset_connection() = 0;
    virtual Status configure(const MixedCanConfig&) = 0;
    virtual Status open() = 0;
    virtual Status close() = 0;
    virtual Status enter_raw_bootloader_mode() = 0;
    virtual Status clear_receive_buffer() = 0;
    virtual Status enter_iso15765_kernel_mode() = 0;
    virtual Status write_iso15765(bytes::ByteView, const ICancellationToken&) = 0;
    virtual Result<std::optional<bytes::Bytes>> read_iso15765(std::chrono::milliseconds, const ICancellationToken&) = 0;
    virtual Status write_raw(const cdbg::CanFrame&, const ICancellationToken&) = 0;
    virtual Result<std::optional<cdbg::CanFrame>> read_raw(std::chrono::milliseconds, const ICancellationToken&) = 0;
};

// Mixed-CAN sibling of IKlineFlashExecutor/ICanFlashExecutor, paired with
// IMixedCanFlashTransport; the same caller-owns-lifecycle contract applies.
// No production consumer yet -- it lands ahead of PR #341's DensoCAN family,
// the first executor expected to implement it.
class IMixedCanFlashExecutor
{
  public:
    using TransportType = IMixedCanFlashTransport;
    using ConfigType = MixedCanConfig;

    virtual ~IMixedCanFlashExecutor() = default;
    virtual Result<MixedCanConfig> transport_setup(const FlashPlan&) const = 0;
    virtual Status before_transport_configure(IMixedCanFlashTransport&, IClock&, const ICancellationToken&) const
    {
        return {};
    }
    virtual Status before_transport_open(const ICancellationToken&) const
    {
        return {};
    }
    virtual Result<FlashExecutionResult> execute(const FlashPlan&, IMixedCanFlashTransport&, IClock&,
                                                 const ICancellationToken&, IEventSink&) = 0;
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
        if (const Status preparation = executor_->before_transport_configure(*transport_, clock, cancellation);
            !preparation.has_value())
        {
            return std::unexpected(preparation.error());
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
