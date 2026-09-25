#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_executor.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "src/backend/flash/ecu/subaru_denso_mc68hc16y5_02_bdm_plan.h"

namespace fastecu::flash
{
namespace
{
using namespace std::chrono_literals;

// Legacy header: serial_read_short/long/extra_long_timeout.
constexpr auto kShortTimeout = 200ms;
constexpr auto kLongTimeout = 800ms;
constexpr auto kExtraLongTimeout = 3000ms;
// read_mem() :157-163 polls up to 50 times, sleeping 100 ms after each poll,
// then sleeps 1 ms per page (:204).
constexpr unsigned kPagePolls = 50;
constexpr auto kPagePollDelay = 100ms;
constexpr auto kInterPageDelay = 1ms;
constexpr std::uint32_t kPageSize = 0x400;
constexpr std::uint32_t kRamStart = 0x20000;
constexpr std::uint32_t kRamEnd = 0x28000;
constexpr std::size_t kUnbounded = std::numeric_limits<std::size_t>::max();

bytes::Bytes ascii(std::string_view text)
{
    bytes::Bytes out;
    out.reserve(text.size());
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
}

Status cancelled_if_requested(const ICancellationToken& cancellation)
{
    return cancellation.cancelled() ? fail(ErrorKind::Cancelled, "MC68HC16Y5 BDM operation cancelled") : Status{};
}

Status write_exact(IKlineFlashTransport& transport, bytes::ByteView request)
{
    auto written = transport.write_raw(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return *written == request.size() ? Status{} : fail(ErrorKind::Disconnected, "short BDM write");
}

// Reads until `want` bytes have arrived, `budget` has elapsed, or a read
// returns nothing. read_raw() returns nothing only after waiting out the whole
// timeout it was given, so an empty read means the window is spent. Legacy
// read_serial_data() instead ran every reply through the K-Line frame parser,
// which cannot pass ASCII or 1 KiB pages (spec: deliberate corrections).
Result<bytes::Bytes> accumulate(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                std::size_t want, std::chrono::milliseconds budget)
{
    const auto deadline = clock.now() + budget;
    bytes::Bytes received;
    while (received.size() < want)
    {
        if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const auto now = clock.now();
        if (now >= deadline)
        {
            break;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        auto chunk = transport.read_raw(remaining, cancellation);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        if (!chunk->has_value() || (*chunk)->empty())
        {
            break;
        }
        received.insert(received.end(), (*chunk)->begin(), (*chunk)->end());
    }
    return received;
}

Status discard(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
               IEventSink& events, std::chrono::milliseconds budget)
{
    auto drained = accumulate(transport, clock, cancellation, kUnbounded, budget);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    if (!drained->empty())
    {
        events.log(LogLevel::Debug, std::format("BDM discarded: {}", bytes::toHex(*drained)));
    }
    return {};
}

// Legacy compares the whole reply with the token (flash_block() :394,
// write_mem() :268, :286); the accumulated reply must equal it exactly.
Status expect_ack(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                  std::string_view token, std::chrono::milliseconds budget)
{
    auto received = accumulate(transport, clock, cancellation, token.size(), budget);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (*received == ascii(token))
    {
        return {};
    }
    if (received->size() < token.size())
    {
        return fail(ErrorKind::Timeout,
                    std::format("BDM bridge did not send {} (received {})", token, bytes::toHex(*received)));
    }
    return fail(ErrorKind::BadResponse,
                std::format("BDM bridge sent {} instead of {}", bytes::toHex(*received), token));
}

// read_mem() :146-175. Legacy replaced its buffer on every poll and appended
// any non-empty remainder; the page now accumulates across polls and must be
// exactly 0x400 bytes (spec: deliberate corrections).
Result<bytes::Bytes> read_page(std::uint32_t address, IKlineFlashTransport& transport, IClock& clock,
                               const ICancellationToken& cancellation)
{
    if (auto written = write_exact(transport, ascii(std::format("rpmem 0x{:08X} 0x{:08X}", address, kPageSize)));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    bytes::Bytes page;
    for (unsigned poll = 0; poll < kPagePolls && page.size() < kPageSize; ++poll)
    {
        auto chunk = accumulate(transport, clock, cancellation, kPageSize - page.size(), kShortTimeout);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        page.insert(page.end(), chunk->begin(), chunk->end());
        if (page.size() > kPageSize)
        {
            return fail(ErrorKind::BadResponse, std::format("BDM page at 0x{:08X} returned {} bytes, expected {}",
                                                            address, page.size(), kPageSize));
        }
        if (auto slept = clock.sleep(kPagePollDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (page.size() != kPageSize)
    {
        return fail(ErrorKind::Timeout,
                    std::format("BDM page at 0x{:08X} returned {} of {} bytes", address, page.size(), kPageSize));
    }
    return page;
}

// read_mem() :82-215.
Result<bytes::Bytes> read_image(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                const ICancellationToken& cancellation, IEventSink& events)
{
    events.log(LogLevel::Info, "Reading ROM from Subaru Denso MC68HC16 with BDM");
    if (auto cleared = discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return std::unexpected(cleared.error());
    }
    const MemoryRegion region = plan.transfer_region();
    const std::uint32_t end = region.start + region.length;
    const int total_pages = static_cast<int>((region.length - (kRamEnd - kRamStart)) / kPageSize);
    int pages_done = 0;
    bytes::Bytes image;
    image.reserve(region.length);
    for (std::uint32_t address = region.start; address < end;)
    {
        if (address == kRamStart)
        {
            // read_mem() :132-144: the RAM block is never requested.
            image.insert(image.end(), kRamEnd - kRamStart, 0xff);
            address = kRamEnd;
            continue;
        }
        if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        auto page = read_page(address, transport, clock, cancellation);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        image.insert(image.end(), page->begin(), page->end());
        events.log(LogLevel::Info, std::format("BDM read addr: 0x{:08X} length: 0x{:08X}", address, kPageSize));
        events.progress(++pages_done, total_pages);
        if (auto slept = clock.sleep(kInterPageDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        address += kPageSize;
    }
    return image;
}
} // namespace

Result<KlineConfig> SubaruDensoMc68hc16y5_02BdmExecutor::transport_setup(const FlashPlan& plan) const
{
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& wire = std::get<SubaruDensoMc68hc16y5_02BdmPlan>(plan.family_plan());
    // execute() :50-54.
    return KlineConfig{
        .baud = wire.baud, .iso14230 = false, .tester_id = 0, .target_id = 0, .parity = KlineParity::None};
}

Status SubaruDensoMc68hc16y5_02BdmExecutor::before_transport_configure(IKlineFlashTransport& transport, IClock&,
                                                                       const ICancellationToken&) const
{
    // Legacy never cleared the header; a session that left ISO-14230 framing
    // on would wrap every ASCII command (spec: deliberate corrections).
    return transport.set_add_iso14230_header(false);
}

Result<FlashExecutionResult>
SubaruDensoMc68hc16y5_02BdmExecutor::execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                             const ICancellationToken& cancellation, IEventSink& events)
{
    if (auto valid = validate_subaru_denso_mc68hc16y5_02_bdm_plan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto cancelled = cancelled_if_requested(cancellation); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (plan.operation() == FlashOperation::Read)
    {
        auto image = read_image(plan, transport, clock, cancellation, events);
        if (!image.has_value())
        {
            return std::unexpected(image.error());
        }
        return FlashExecutionResult{
            .operation = FlashOperation::Read, .read_bytes = std::move(*image), .rom_id = std::nullopt};
    }
    // Replaced by the kernel bootstrap in Task 3.
    return fail(ErrorKind::Unsupported, "MC68HC16Y5 BDM kernel bootstrap is not implemented");
}
} // namespace fastecu::flash
