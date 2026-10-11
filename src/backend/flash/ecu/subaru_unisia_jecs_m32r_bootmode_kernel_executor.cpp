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

Status CancelledIfRequested(const ICancellationToken& cancellation, std::string_view where)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::format("cancelled {}", where));
    }
    return {};
}

Status Upload(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
              const ICancellationToken& cancellation, IEventSink& events)
{
    if (Status cancelled = CancelledIfRequested(cancellation, "before boot mode lines"); !cancelled.has_value())
    {
        return cancelled;
    }
    // execute() :64-67.
    events.Log(LogLevel::kInfo, "Set programming voltage +12v to Line End Check 1 and MOD1 to Line End Check 2");
    if (Status raised = transport.EnableBootModeLines(); !raised.has_value())
    {
        return raised;
    }

    // upload_kernel() :318-335: unframed 128-byte chunks, no reply read per
    // chunk.
    events.Log(LogLevel::kInfo, "Uploading kernel, please wait...");
    const bytes::Bytes& kernel = plan.ImageOrEmpty();
    const auto chunks = static_cast<int>(kernel.size() / kChunk);
    for (int index = 0; index < chunks; ++index)
    {
        if (Status cancelled = CancelledIfRequested(cancellation, "during kernel upload"); !cancelled.has_value())
        {
            return cancelled;
        }
        const bytes::ByteView data = bytes::ByteView(kernel).subspan(static_cast<std::size_t>(index) * kChunk, kChunk);
        auto written = transport.Write(data);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        if (*written != data.size())
        {
            return Fail(ErrorKind::kDisconnected, "short K-Line write during kernel upload");
        }
        events.Progress(index + 1, chunks);
    }

    // upload_kernel() :338-340: legacy slept, read once with the short
    // timeout, and discarded whatever came back. Nothing here proves the
    // kernel runs; attempt 2's first gated reply does.
    if (Status settled = clock.Sleep(kSettle, cancellation); !settled.has_value())
    {
        return settled;
    }
    auto trailing = transport.Read(kSettleRead, cancellation);
    if (!trailing.has_value())
    {
        return std::unexpected(trailing.error());
    }
    if (trailing->has_value())
    {
        events.Log(LogLevel::kDebug, std::format("Discarded after kernel upload: {}", bytes::ToHex(**trailing)));
    }
    return {};
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsM32rBootModeKernelExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rBootmodePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    // execute() :52-60.
    KlineConfig config =
        NonIso14230KlineConfigFrom(std::get<SubaruUnisiaJecsM32rBootModeKernelPlan>(plan.FamilyPlan()));
    config.parity = KlineParity::kEven; // execute() :57
    return config;
}

Status SubaruUnisiaJecsM32rBootModeKernelExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                            const ICancellationToken&) const
{
    // execute() :52-53.
    if (Status reset = transport.ResetConnection(); !reset.has_value())
    {
        return reset;
    }
    return transport.SetAddIso14230Header(false);
}

Result<FlashExecutionResult> SubaruUnisiaJecsM32rBootModeKernelExecutor::Execute(const FlashPlan& plan,
                                                                                 IKlineFlashTransport& transport,
                                                                                 IClock& clock,
                                                                                 const ICancellationToken& cancellation,
                                                                                 IEventSink& events)
{
    if (Status match = CheckFamily(plan, FlashFamily::kSubaruUnisiaJecsM32rBootModeKernel); !match.has_value())
    {
        return std::unexpected(match.error());
    }
    if (Status valid = ValidateSubaruUnisiaJecsM32rBootmodePlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const Status uploaded = Upload(plan, transport, clock, cancellation, events);
    // Legacy dropped the boot mode lines implicitly, by closing the port in
    // write_mem()'s reset_connection() (:361); the explicit drop here is the
    // same on every adapter, and unconditional so a failed or cancelled
    // upload never leaves VPP/MOD1 raised.
    events.Log(LogLevel::kDebug, "Removing boot mode voltages from Line End Check 1 and 2");
    const Status dropped = transport.DisableLecLines();
    if (!uploaded.has_value())
    {
        return std::unexpected(uploaded.error());
    }
    if (!dropped.has_value())
    {
        return std::unexpected(dropped.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
