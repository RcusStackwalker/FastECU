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

bytes::Bytes Ascii(std::string_view text)
{
    bytes::Bytes out;
    out.reserve(text.size());
    for (const char c : text)
    {
        out.push_back(static_cast<bytes::Byte>(c));
    }
    return out;
}

Status CancelledIfRequested(const ICancellationToken& cancellation)
{
    return cancellation.Cancelled() ? Fail(ErrorKind::kCancelled, "MC68HC16Y5 BDM operation cancelled") : Status{};
}

Status WriteExact(IKlineFlashTransport& transport, bytes::ByteView request)
{
    auto written = transport.WriteRaw(request);
    if (!written.has_value())
    {
        return std::unexpected(written.error());
    }
    return *written == request.size() ? Status{} : Fail(ErrorKind::kDisconnected, "short BDM write");
}

// Reads until `want` bytes have arrived, `budget` has elapsed, or a read
// returns nothing. read_raw() returns nothing only after waiting out the whole
// timeout it was given, so an empty read means the window is spent. Legacy
// read_serial_data() instead ran every reply through the K-Line frame parser,
// which cannot pass ASCII or 1 KiB pages (spec: deliberate corrections).
Result<bytes::Bytes> Accumulate(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                                std::size_t want, std::chrono::milliseconds budget)
{
    const auto deadline = clock.Now() + budget;
    bytes::Bytes received;
    while (received.size() < want)
    {
        if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        const auto now = clock.Now();
        if (now >= deadline)
        {
            break;
        }
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
        auto chunk = transport.ReadRaw(remaining, cancellation);
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

Status Discard(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
               IEventSink& events, std::chrono::milliseconds budget)
{
    auto drained = Accumulate(transport, clock, cancellation, kUnbounded, budget);
    if (!drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    if (!drained->empty())
    {
        events.Log(LogLevel::kDebug, std::format("BDM discarded: {}", bytes::ToHex(*drained)));
    }
    return {};
}

// Legacy compares the whole reply with the token (flash_block() :394,
// write_mem() :268, :286); the accumulated reply must equal it exactly.
Status ExpectAck(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                 std::string_view token, std::chrono::milliseconds budget)
{
    auto received = Accumulate(transport, clock, cancellation, token.size(), budget);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (*received == Ascii(token))
    {
        return {};
    }
    if (received->size() < token.size())
    {
        return Fail(ErrorKind::kTimeout,
                    std::format("BDM bridge did not send {} (received {})", token, bytes::ToHex(*received)));
    }
    return Fail(ErrorKind::kBadResponse,
                std::format("BDM bridge sent {} instead of {}", bytes::ToHex(*received), token));
}

// read_mem() :146-175. Legacy replaced its buffer on every poll and appended
// any non-empty remainder; the page now accumulates across polls and must be
// exactly 0x400 bytes (spec: deliberate corrections).
Result<bytes::Bytes> ReadPage(std::uint32_t address, IKlineFlashTransport& transport, IClock& clock,
                              const ICancellationToken& cancellation)
{
    if (auto written = WriteExact(transport, Ascii(std::format("rpmem 0x{:08X} 0x{:08X}", address, kPageSize)));
        !written.has_value())
    {
        return std::unexpected(written.error());
    }
    bytes::Bytes page;
    for (unsigned poll = 0; poll < kPagePolls && page.size() < kPageSize; ++poll)
    {
        auto chunk = Accumulate(transport, clock, cancellation, kPageSize - page.size(), kShortTimeout);
        if (!chunk.has_value())
        {
            return std::unexpected(chunk.error());
        }
        page.insert(page.end(), chunk->begin(), chunk->end());
        if (page.size() > kPageSize)
        {
            return Fail(ErrorKind::kBadResponse, std::format("BDM page at 0x{:08X} returned {} bytes, expected {}",
                                                             address, page.size(), kPageSize));
        }
        if (auto slept = clock.Sleep(kPagePollDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    if (page.size() != kPageSize)
    {
        return Fail(ErrorKind::kTimeout,
                    std::format("BDM page at 0x{:08X} returned {} of {} bytes", address, page.size(), kPageSize));
    }
    return page;
}

// read_mem() :82-215.
Result<bytes::Bytes> ReadImage(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                               const ICancellationToken& cancellation, IEventSink& events)
{
    events.Log(LogLevel::kInfo, "Reading ROM from Subaru Denso MC68HC16 with BDM");
    if (auto cleared = Discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return std::unexpected(cleared.error());
    }
    const MemoryRegion region = plan.TransferRegion();
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
        if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
        {
            return std::unexpected(cancelled.error());
        }
        auto page = ReadPage(address, transport, clock, cancellation);
        if (!page.has_value())
        {
            return std::unexpected(page.error());
        }
        image.insert(image.end(), page->begin(), page->end());
        events.Log(LogLevel::kInfo, std::format("BDM read addr: 0x{:08X} length: 0x{:08X}", address, kPageSize));
        events.Progress(++pages_done, total_pages);
        if (auto slept = clock.Sleep(kInterPageDelay, cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
        address += kPageSize;
    }
    // Bytes the bridge sends after this last page's poll are never read:
    // there is no next poll left to see them.
    return image;
}

constexpr std::size_t kUploadChunk = kSubaruDensoMc68hc16y5_02BdmUploadChunk;
constexpr std::string_view kAckCommand = "ACK_CMD_WDMEM";
constexpr std::string_view kAckWrite = "ACK_WR";

Status SendAndAck(IKlineFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                  bytes::ByteView request, std::string_view token, std::chrono::milliseconds budget)
{
    if (auto written = WriteExact(transport, request); !written.has_value())
    {
        return written;
    }
    return ExpectAck(transport, clock, cancellation, token, budget);
}

// write_mem() :296-305 reads and logs these replies without checking them;
// the bridge's replies are unknown, so they stay ungated (spec appendix).
Status LogReply(std::string_view command, IKlineFlashTransport& transport, IClock& clock,
                const ICancellationToken& cancellation, IEventSink& events)
{
    auto reply = Accumulate(transport, clock, cancellation, kUnbounded, kLongTimeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    events.Log(LogLevel::kInfo, std::format("BDM {} reply: {}", command, bytes::ToHex(*reply)));
    return {};
}

// write_mem() :217-350 and flash_block() :352-474. Uploads the padded kernel to
// RAM, enables SCIB, sets PC/SP and starts the kernel. The ROM is not written.
Status BootstrapKernel(bytes::ByteView kernel, IKlineFlashTransport& transport, IClock& clock,
                       const ICancellationToken& cancellation, IEventSink& events)
{
    events.Log(LogLevel::kInfo, "Uploading kernel to Subaru Denso MC68HC16 RAM with BDM");
    // write_mem() :226.
    if (auto cleared = Discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // flash_block() :378-399.
    events.Log(LogLevel::kInfo, std::format("Writing block: 00 at address 0x{:08X}", kRamStart));
    if (auto started = SendAndAck(transport, clock, cancellation,
                                  Ascii(std::format("wdmem 0x{:08X} 0x{:08X}", kRamStart, kernel.size())), kAckCommand,
                                  kExtraLongTimeout);
        !started.has_value())
    {
        return started;
    }
    // flash_block() :402-468.
    const std::size_t chunks = kernel.size() / kUploadChunk;
    for (std::size_t index = 0; index < chunks; ++index)
    {
        if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
        {
            return cancelled;
        }
        if (auto acked = SendAndAck(transport, clock, cancellation, kernel.subspan(index * kUploadChunk, kUploadChunk),
                                    kAckWrite, kLongTimeout);
            !acked.has_value())
        {
            return acked;
        }
        events.Progress(static_cast<int>(index + 1), static_cast<int>(chunks));
    }
    events.Log(LogLevel::kInfo, "Block write complete.");
    // flash_block() :470.
    if (auto cleared = Discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :258-275: enable SCIB (the kernel sets 62500 baud itself).
    if (auto scib =
            SendAndAck(transport, clock, cancellation, Ascii("wdmem 0xFFC28 0x4"), kAckCommand, kExtraLongTimeout);
        !scib.has_value())
    {
        return scib;
    }
    if (auto cleared = Discard(transport, clock, cancellation, events, kLongTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :277-294.
    if (auto scib =
            SendAndAck(transport, clock, cancellation, bytes::Bytes{0x00, 0x0d, 0x00, 0x0c}, kAckWrite, kLongTimeout);
        !scib.has_value())
    {
        return scib;
    }
    if (auto cleared = Discard(transport, clock, cancellation, events, kShortTimeout); !cleared.has_value())
    {
        return cleared;
    }
    // write_mem() :296-305.
    if (auto written = WriteExact(transport, Ascii("wpcsp")); !written.has_value())
    {
        return written;
    }
    for (int reply = 0; reply < 2; ++reply)
    {
        if (auto logged = LogReply("wpcsp", transport, clock, cancellation, events); !logged.has_value())
        {
            return logged;
        }
    }
    // write_mem() :317-323. Once `go` is sent the kernel is running: nothing
    // after it can fail or cancel the operation.
    if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
    {
        return cancelled;
    }
    if (auto written = WriteExact(transport, Ascii("go")); !written.has_value())
    {
        return written;
    }
    if (auto reply = Accumulate(transport, clock, cancellation, kUnbounded, kLongTimeout); reply.has_value())
    {
        events.Log(LogLevel::kInfo, std::format("BDM go reply: {}", bytes::ToHex(*reply)));
    }
    else
    {
        events.Log(LogLevel::kWarning, std::format("BDM go reply not read: {}", reply.error().detail));
    }
    return {};
}
} // namespace

Result<KlineConfig> SubaruDensoMc68hc16y5_02BdmExecutor::TransportSetup(const FlashPlan& plan) const
{
    if (auto valid = ValidateSubaruDensoMc68hc16y502BdmPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    const auto& wire = std::get<SubaruDensoMc68hc16y5_02BdmPlan>(plan.FamilyPlan());
    // execute() :50-54.
    return KlineConfig{
        .baud = wire.baud, .iso14230 = false, .tester_id = 0, .target_id = 0, .parity = KlineParity::kNone};
}

Status SubaruDensoMc68hc16y5_02BdmExecutor::BeforeTransportConfigure(IKlineFlashTransport& transport, IClock&,
                                                                     const ICancellationToken&) const
{
    // Legacy never cleared the header; a session that left ISO-14230 framing
    // on would wrap every ASCII command (spec: deliberate corrections).
    return transport.SetAddIso14230Header(false);
}

Result<FlashExecutionResult>
SubaruDensoMc68hc16y5_02BdmExecutor::Execute(const FlashPlan& plan, IKlineFlashTransport& transport, IClock& clock,
                                             const ICancellationToken& cancellation, IEventSink& events)
{
    if (auto valid = ValidateSubaruDensoMc68hc16y502BdmPlan(plan); !valid.has_value())
    {
        return std::unexpected(valid.error());
    }
    if (auto cancelled = CancelledIfRequested(cancellation); !cancelled.has_value())
    {
        return std::unexpected(cancelled.error());
    }
    if (plan.Operation() == FlashOperation::kRead)
    {
        auto image = ReadImage(plan, transport, clock, cancellation, events);
        if (!image.has_value())
        {
            return std::unexpected(image.error());
        }
        return FlashExecutionResult{
            .operation = FlashOperation::kRead, .read_bytes = std::move(*image), .rom_id = std::nullopt};
    }
    if (auto booted = BootstrapKernel(plan.ImageOrEmpty(), transport, clock, cancellation, events); !booted.has_value())
    {
        return std::unexpected(booted.error());
    }
    return FlashExecutionResult{
        .operation = FlashOperation::kWrite, .read_bytes = std::nullopt, .rom_id = std::nullopt};
}
} // namespace fastecu::flash
