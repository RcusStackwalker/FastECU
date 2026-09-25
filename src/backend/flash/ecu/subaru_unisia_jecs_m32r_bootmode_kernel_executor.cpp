#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_kernel_executor.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <string_view>

#include "src/backend/flash/ecu/subaru_unisia_jecs_m32r_bootmode_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

// Legacy flash_ecu_subaru_unisia_jecs_m32r_bootmode_operation.cpp.
constexpr std::size_t kChunk = 0x80; // upload_kernel() :312-315, :320-330
constexpr auto kSettle = 500ms;      // upload_kernel() :338
constexpr auto kSettleRead = 200ms;  // upload_kernel() :339 (serial_read_short_timeout)

Status cancelled_if_requested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::format("cancelled {}", where));
    }
    return {};
}

Status upload(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
              const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status cancelled = cancelled_if_requested(cancellation, "before boot mode lines"); !cancelled.has_value())
    {
        return cancelled;
    }
    // execute() :64-67.
    events.log(LogLevel::Info, "Set programming voltage +12v to Line End Check 1 and MOD1 to Line End Check 2");
    if (Status raised = transport.enable_boot_mode_lines(); !raised.has_value())
    {
        return raised;
    }

    // upload_kernel() :318-335: unframed 128-byte chunks, no reply read per
    // chunk.
    events.log(LogLevel::Info, "Uploading kernel, please wait...");
    const bytes::Bytes& kernel = *plan.image();
    const auto chunks = static_cast<int>(kernel.size() / kChunk);
    for (int index = 0; index < chunks; ++index)
    {
        if (Status cancelled = cancelled_if_requested(cancellation, "during kernel upload"); !cancelled.has_value())
        {
            return cancelled;
        }
        const bytes::ByteView data = bytes::ByteView(kernel).subspan(static_cast<std::size_t>(index) * kChunk, kChunk);
        auto written = transport.write(data);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (*written != data.size())
        {
            return fail(ErrorKind::Disconnected, "short K-Line write during kernel upload");
        }
        events.progress(index + 1, chunks);
    }

    // upload_kernel() :338-340: legacy slept, read once with the short
    // timeout, and discarded whatever came back. Nothing here proves the
    // kernel runs; attempt 2's first gated reply does.
    if (Status settled = clock.sleep(kSettle, cancellation); !settled.has_value())
    {
        return settled;
    }
    auto trailing = transport.read(kSettleRead, cancellation);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        events.log(LogLevel::Debug, std::format("Discarded after kernel upload: {}", bytes::toHex(**trailing)));
    }
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeKernelExecutor::transport_setup(const FlashPlan& plan) const
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :52-60.
    KlineConfig config =
        non_iso14230_kline_config_from(std::get<SubaruUnisiaJecsM32rBootModeKernelPlan>(plan.family_plan()));
    config.parity = KlineParity::Even; // execute() :57
    return config;
}

Status SubaruUnisiaJecsM32rBootModeKernelExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                              const ICancellationToken&) const
{
    // execute() :52-53.
    if (Status reset = transport.reset_connection(); !reset.has_value())
    {
        return reset;
    }
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rBootModeKernelExecutor::execute(const FlashPlan& plan,
                                                                                 IKlineFlashTransport& transport,
                                                                                 IClock& clock,
                                                                                 const ICancellationToken& cancellation,
                                                                                 IEventSink& events)
{
    if (Status match = check_family(plan, FlashFamily::SubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = validate_subaru_unisia_jecs_m32r_bootmode_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const Status uploaded = upload(plan, transport, clock, cancellation, events);
    // Legacy dropped the boot mode lines implicitly, by closing the port in
    // write_mem()'s reset_connection() (:361); the explicit drop here is the
    // same on every adapter, and unconditional so a failed or cancelled
    // upload never leaves VPP/MOD1 raised.
    events.log(LogLevel::Debug, "Removing boot mode voltages from Line End Check 1 and 2");
    const Status dropped = transport.disable_lec_lines();
    if (!uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{.operation = FlashOperation::Write, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
