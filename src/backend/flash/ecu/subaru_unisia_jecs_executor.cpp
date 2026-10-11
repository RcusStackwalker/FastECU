#include "src/backend/flash/ecu/subaru_unisia_jecs_executor.h"

#include <array>
#include <chrono>
#include <utility>

#include "src/backend/flash/ecu/subaru_unisia_jecs_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

constexpr std::array<bytes::Byte, 4> kWakeup{0x78, 0x12, 0x34, 0x00};
constexpr auto kWakeupDelay = 500ms;
constexpr auto kWakeupFlushTimeout = 1000ms;
constexpr auto kAddressDelay = 45ms;
constexpr auto kInterAddressDelay = 1ms;
constexpr auto kRawReadTimeout = 5ms;
constexpr unsigned kReadsBeforeRetry = 100;
constexpr std::uint32_t kRomSize = 0x10000;

struct RawReadState
{
    bytes::Bytes pending;
    bool synchronized = false;
    unsigned tuples_since_sync = 0;
};

Status CancelledIfRequested(const ICancellationToken& cancellation)
{
    return cancellation.Cancelled() ? Fail(ErrorKind::kCancelled, "cancelled while reading Unisia Jecs ROM") : Status{};
}

bytes::Bytes RequestFor(std::uint16_t address)
{
    return {0x78, static_cast<bytes::Byte>(address >> 8U), static_cast<bytes::Byte>(address), 0x00};
}

Status WriteExact(IKlineFlashTransport& transport, bytes::ByteView request, bool raw)
{
    auto written = raw ? transport.WriteRaw(request) : transport.Write(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return *written == request.size() ? Status{} : Fail(ErrorKind::kDisconnected, "short K-Line write");
}

Result<bytes::Byte> ReadAddress(std::uint16_t address, RawReadState& state, IKlineFlashTransport& transport,
                                IClock& clock, const ICancellationToken& cancellation)
{
    const bytes::Bytes request = RequestFor(address);
    if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (auto written = WriteExact(transport, request, true); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (auto slept = clock.Sleep(kAddressDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }

    const auto high = static_cast<bytes::Byte>(address >> 8U);
    const auto low = static_cast<bytes::Byte>(address);
    unsigned read_quanta = 0;
    while (true)
    {
        if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        while (state.pending.size() >= 3U)
        {
            ++state.tuples_since_sync;
            if (state.pending[0] == high && state.pending[1] == low)
            {
                const bytes::Byte value = state.pending[2];
                state.pending.erase(state.pending.begin(), state.pending.begin() + 3);
                state.synchronized = true;
                state.tuples_since_sync = 0;
                return value;
            }
            const std::size_t discard = state.synchronized ? 3U : 1U;
            state.pending.erase(state.pending.begin(), state.pending.begin() + static_cast<std::ptrdiff_t>(discard));
            if (state.tuples_since_sync > 10U)
            {
                state.synchronized = false;
                state.tuples_since_sync = 0;
            }
        }
        if (read_quanta == kReadsBeforeRetry)
        {
            if (auto written = WriteExact(transport, request, true); !written.has_value())
            {
                return std::unexpected(written.error());
            }
            read_quanta = 0;
        }

        auto chunk = transport.ReadRaw(kRawReadTimeout, cancellation);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        if (chunk->has_value())
        {
            state.pending.insert(state.pending.end(), (**chunk).begin(), (**chunk).end());
        }
        ++read_quanta;
    }
}
} // namespace

Result<KlineConfig> SubaruUnisiaJecsExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (auto valid = ValidateSubaruUnisiaJecsPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    return KlineConfig{.baud = 1953, .iso14230 = false, .tester_id = 0, .target_id = 0, .parity = KlineParity::kEven};
}

Result<bytes::Bytes> SubaruUnisiaJecsExecutor::ReadRange(std::uint32_t begin, std::uint32_t end,
                                                         IKlineFlashTransport& transport, IClock& clock,
                                                         const ICancellationToken& cancellation, IEventSink& events)
{
    bytes::Bytes image;
    image.reserve(end - begin);
    RawReadState state;
    for (std::uint32_t address = begin; address < end; ++address)
    {
        auto value = ReadAddress(static_cast<std::uint16_t>(address), state, transport, clock, cancellation);
        if (!value.has_value())
        {
            return std::unexpected(value.error());
        }
        image.push_back(*value);
        events.Progress(static_cast<int>(address - begin + 1U), static_cast<int>(end - begin));
        if (auto slept = clock.Sleep(kInterAddressDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return image;
}

Result<FlashExecutionResult> SubaruUnisiaJecsExecutor::Execute(const FlashPlan& plan, IKlineFlashTransport& transport,
                                                               IClock& clock, const ICancellationToken& cancellation,
                                                               IEventSink& events)
{
    if (auto valid = ValidateSubaruUnisiaJecsPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (auto written = WriteExact(transport, kWakeup, false); !written.has_value())
    {
        return std::unexpected(written.error());
    }
    if (auto slept = clock.Sleep(kWakeupDelay, cancellation); !slept.has_value())
    {
        return std::unexpected(slept.error());
    }
    auto flushed = transport.Read(kWakeupFlushTimeout, cancellation);
    if (!flushed.has_value())
    {
        return std::unexpected(flushed.error());
    }
    auto image = ReadRange(0, kRomSize, transport, clock, cancellation, events);
    if (!image.has_value())
    {
        return std::unexpected(image.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kRead, .read_bytes = std::move(*image), .rom_id = std::nullopt};
}
} // namespace fastecu::flash
