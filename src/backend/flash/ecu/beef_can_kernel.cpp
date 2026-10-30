#include "src/backend/flash/ecu/beef_can_kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/transfer_progress.h"

namespace fastecu::flash
{
namespace
{

constexpr std::size_t kUploadBlockSize = 128;
constexpr std::uint32_t kReadPageSize = 0x400;
constexpr std::chrono::milliseconds kReadTimeout{2000};

Result<bytes::Bytes> PreparePayload(bytes::ByteView kernel, std::string_view family)
{
    const std::size_t padded_to_word = (kernel.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_to_word + kUploadBlockSize - 1U) / kUploadBlockSize;
    if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / kUploadBlockSize)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload size is invalid", family));
    }
    bytes::Bytes plain(kernel.begin(), kernel.end());
    plain.resize(block_count * kUploadBlockSize - 4U, bytes::Byte{0});
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < plain.size(); offset += 4)
    {
        sum += bytes::ReadU32Be(plain, offset);
    }
    bytes::AppendU32Be(plain, 0x5AA5A55AU - sum);
    bytes::Bytes encrypted = DensoEncryptRom(plain);
    if (encrypted.size() >= 0x1000000U)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload exceeds the 24-bit wire length", family));
    }
    return encrypted;
}

void LogDescribedFailure(IEventSink& events, const DensoBeefCanProfile& profile, const Error& failure)
{
    if (failure.kind == ErrorKind::kTimeout)
    {
        events.Log(LogLevel::kError, "No valid response from ECU");
    }
    else if (failure.kind == ErrorKind::kBadResponse)
    {
        events.Log(LogLevel::kError, std::format("Wrong response from {}: {}", profile.device, failure.detail));
    }
}

struct KernelContext
{
    const ICancellationToken& cancellation;
    IEventSink& events;
    IClock& clock;
    ICanFlashTransport& transport;
    uds::IUdsChannel& channel;
    const DensoBeefCanProfile& profile;
};

// The two-byte frame marker every BEEF-protocol request and reply opens with.
// Byte-identical across all three consumers.
constexpr std::uint32_t kKernelStartComm = 0xBEEF;

// A parsed BEEF reply: its opcode and the payload the declared length covers.
struct BeefMessage
{
    bytes::Byte opcode;
    bytes::ByteView payload;
};

Result<BeefMessage> ParseBeef(bytes::ByteView pdu)
{
    if (pdu.size() < 5)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response envelope");
    }
    if (bytes::ReadU16Be(pdu) != kKernelStartComm)
    {
        return Fail(ErrorKind::kBadResponse, "wrong BEEF response marker");
    }
    const std::uint16_t declared = bytes::ReadU16Be(pdu, 2);
    // The legacy serial reader can return a complete ISO payload with
    // transport padding or a coalesced trailing frame.  Its checks only use
    // the declared BEEF body, so retain that tolerance while bounding every
    // indexed field.  The view exposed to callers is limited to the declared
    // opcode-plus-payload body.
    if (declared < 1 || static_cast<std::size_t>(declared) + 4 > pdu.size())
    {
        return Fail(ErrorKind::kBadResponse, "invalid BEEF response length");
    }
    return BeefMessage{.opcode = pdu[4], .payload = pdu.subspan(5, declared - 1)};
}

Status CancelledIfRequested(const KernelContext& ctx, std::string_view detail)
{
    if (ctx.cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::string(detail));
    }
    return {};
}

// Warm exchanges have no post-send delay.
Result<bytes::Bytes> ChannelRequest(KernelContext& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "cancelled before CAN request"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    auto received = ctx.channel.Receive(timeout, ctx.cancellation);
    if (!received.has_value())
    {
        return std::unexpected(received.error());
    }
    if (!received->has_value())
    {
        return Fail(ErrorKind::kTimeout, "no CAN response within the read timeout");
    }
    return std::move(**received);
}

// Legacy reported a floor of one millisecond for any completed transfer, so a
// sub-millisecond phase never renders as "0 ms".
std::uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start,
                                  std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

// A BEEF request whose reply must carry `opcode | 0x40` and at least
// `minimum_payload` payload bytes; returns that payload.
Result<bytes::Bytes> Exchange(KernelContext& ctx, bytes::Byte opcode, bytes::ByteView payload,
                              std::size_t minimum_payload, std::chrono::milliseconds timeout)
{
    const auto request = bytes::ComposeBe(std::uint16_t{kKernelStartComm},
                                          static_cast<std::uint16_t>(payload.size() + 1), opcode, payload);
    Result<bytes::Bytes> reply = ChannelRequest(ctx, request, timeout);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    Result<BeefMessage> parsed = ParseBeef(*reply);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    const bytes::Byte expected = static_cast<bytes::Byte>(opcode | 0x40U);
    if (parsed->opcode != expected)
    {
        return Fail(ErrorKind::kBadResponse, std::format("unexpected BEEF response opcode 0x{:02x}, expected 0x{:02x}",
                                                         parsed->opcode, expected));
    }
    if (parsed->payload.size() < minimum_payload)
    {
        return Fail(ErrorKind::kBadResponse, "short BEEF response payload");
    }
    return bytes::Bytes(parsed->payload.begin(), parsed->payload.end());
}

Result<std::uint32_t> QueryCrc(KernelContext& ctx, const MemoryRegion& block)
{
    Result<bytes::Bytes> reply =
        Exchange(ctx, 0x02, bytes::ComposeBe(block.start, bytes::Byte{0x00}, bytes::U24{block.length}), 4,
                 std::chrono::milliseconds{3000});
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    // The raw 200 ms read following a CRC reply tolerates stale content and
    // nonterminal errors. Only cancellation and disconnection are actionable.
    if (const Status checkpoint = CancelledIfRequested(ctx, "CRC stale-frame drain cancelled"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    Result<std::optional<bytes::Bytes>> stale = ctx.transport.Read(std::chrono::milliseconds{200}, ctx.cancellation);
    if (!stale.has_value() &&
        (stale.error().kind == ErrorKind::kCancelled || stale.error().kind == ErrorKind::kDisconnected))
    {
        return std::unexpected(stale.error());
    }
    return bytes::ReadU32Be(*reply);
}

Result<std::vector<std::size_t>> CompareBlocks(KernelContext& ctx, std::span<const MemoryRegion> blocks,
                                               std::uint32_t image_base, bytes::ByteView image, PhaseReporter *progress,
                                               bool after_reflash)
{
    std::vector<std::size_t> changed;
    LogInfo(ctx, after_reflash ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                               : "--- Comparing ECU flash memory pages to image file ---");
    LogInfo(ctx, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "ROM compare cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const MemoryRegion block = blocks[index];
        if (block.start < image_base ||
            static_cast<std::uint64_t>(block.start - image_base) + block.length > image.size())
        {
            return Fail(ErrorKind::kInvalidConfig,
                        std::format("{} CRC block is outside the image", ctx.profile.family));
        }
        LogInfo(ctx, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        Result<std::uint32_t> ecu_crc = QueryCrc(ctx, block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::size_t image_offset = block.start - image_base;
        const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(image_offset, block.length));
        LogDebug(ctx, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
        LogInfo(ctx, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
        const bool different = *ecu_crc != image_crc;
        LogInfo(ctx, different ? "\tNO" : "\tYES");
        if (different)
        {
            changed.push_back(index);
        }
        if (progress != nullptr)
        {
            progress->Update(static_cast<int>(index + 1));
        }
        if (const Status slept = ctx.clock.Sleep(std::chrono::milliseconds{5}, ctx.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }

    LogInfo(ctx, "Different blocks : ");
    for (const auto index : changed)
    {
        LogInfo(ctx, std::format("{}, ", index));
    }
    LogInfo(ctx, std::format(" (total: {})", changed.size()));
    return changed;
}

Status InitializeFlash(KernelContext& ctx, bool test_write)
{
    for (const auto& [opcode, label] :
         {std::pair{bytes::Byte{0x05}, "Check max message length"}, {bytes::Byte{0x06}, "Check flashblock size"}})
    {
        LogInfo(ctx, label);
        const auto reply = Exchange(ctx, opcode, {}, 4, ctx.profile.init_timeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const std::uint32_t size = bytes::ReadU32Be(*reply);
        LogInfo(ctx, ctx.profile.upper_hex_sizes ? std::format(": 0x{:04X}", size) : std::format(": 0x{:04x}", size));
    }

    LogInfo(ctx, test_write ? "Test write mode on, no actual flash write is performed"
                            : "Test write mode off, perform actual flash write");
    return Exchange(ctx, test_write ? bytes::Byte{0x21} : bytes::Byte{0x20}, {}, 0, ctx.profile.init_timeout)
        .transform([&](const bytes::Bytes&) { LogError(ctx, "Flash mode succesfully set"); });
}

constexpr std::size_t kBeefFlashBufferSize = 0x200;
constexpr std::size_t kBeefFlashCommitSize = 0x1000;
constexpr bytes::Byte kBeefProgVolt = 0x04;
constexpr bytes::Byte kBeefWriteBuffer = 0x22;
constexpr bytes::Byte kBeefValidateBuffer = 0x23;
constexpr bytes::Byte kBeefCommitBuffer = 0x24;
constexpr bytes::Byte kBeefBlankPage = 0x25;
constexpr std::chrono::milliseconds kBeefWriteTimeout{800};
constexpr std::chrono::milliseconds kBeefVoltageTimeout{500};
constexpr std::chrono::milliseconds kBeefEraseTimeout{3000};

Status ValidateFlashBlock(const KernelContext& ctx, bytes::ByteView image, std::uint32_t image_base,
                          const MemoryRegion& block)
{
    if (block.length == 0 || block.length % kBeefFlashBufferSize != 0 || block.length % kBeefFlashCommitSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} flash block is not aligned to {}", ctx.profile.family,
                                ctx.profile.upper_hex_sizes ? "the portable write windows" : "write windows"));
    }
    if (block.start < image_base)
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} flash block starts before the image", ctx.profile.family));
    }
    const std::size_t image_offset = block.start - image_base;
    if (image_offset + block.length > image.size())
    {
        return Fail(ErrorKind::kInvalidConfig,
                    std::format("{} flash block extends beyond the image", ctx.profile.family));
    }

    return {};
}

void LogFlashRegion(const KernelContext& ctx, std::string_view label, const MemoryRegion& block)
{
    LogInfo(ctx, ctx.profile.upper_hex_sizes
                     ? std::format("{}: 0x{:08X} len: 0x{:08X}", label, block.start, block.length)
                     : std::format("{}: 0x{:08x} len: 0x{:08x}", label, block.start, block.length));
}

Status CommitBuffer(KernelContext& ctx, bytes::ByteView window, std::uint32_t address, bool test_write)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "flash commit cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    LogInfo(ctx, "Flash buffer write complete... ");
    const std::uint32_t image_crc = fastecu::checksum::Crc32(window);
    // TCU's legacy CRC logs keep eight digits; ECU logs omit leading zeroes.
    const std::string crc =
        ctx.profile.upper_hex_sizes ? std::format("{:08x}", image_crc) : std::format("{:x}", image_crc);
    LogDebug(ctx, std::format("Image CRC32: 0x{}", crc));
    LogInfo(ctx, std::format("{} flash addr: 0x{:x}", test_write ? "Validate" : "Committ", address));
    LogInfo(ctx, std::format(" len: 0x{:x}", kBeefFlashCommitSize));
    LogInfo(ctx, std::format(" crc32: 0x{}", crc));
    const auto commit = bytes::ComposeBe(address, std::uint16_t{kBeefFlashCommitSize}, image_crc);
    return Exchange(ctx, test_write ? kBeefValidateBuffer : kBeefCommitBuffer, commit, 0, kBeefEraseTimeout)
        .transform([](const bytes::Bytes&) {});
}

Status FlashBlock(KernelContext& ctx, bytes::ByteView image, std::uint32_t image_base, const MemoryRegion& block,
                  bool test_write, std::size_t& flash_bytes_index, std::size_t flash_bytes_count,
                  PhaseReporter& progress)
{
    if (const auto valid = ValidateFlashBlock(ctx, image, image_base, block); !valid.has_value())
    {
        return valid;
    }
    const std::size_t image_offset = block.start - image_base;
    LogFlashRegion(ctx, "Flash page erase addr", block);
    LogInfo(ctx, "Erasing flash page...");
    Result<bytes::Bytes> erased = Exchange(ctx, kBeefBlankPage, bytes::ComposeBe(block.start), 0, kBeefEraseTimeout);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    LogInfo(ctx, " erased");

    auto previous_time = ctx.clock.Now();
    LogFlashRegion(ctx, "Start flash write addr", block);
    for (std::uint32_t offset = 0; offset < block.length; offset += kBeefFlashBufferSize)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "flash write cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::uint32_t address = block.start + offset;
        const bytes::ByteView chunk = image.subspan(image_offset + offset, kBeefFlashBufferSize);
        Result<bytes::Bytes> written =
            Exchange(ctx, kBeefWriteBuffer, bytes::ComposeBe(address, chunk), 0, kBeefWriteTimeout);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        LogDebug(ctx, "Data written to flash buffer");

        const std::uint32_t percent = 100U * offset / block.length;
        const auto now = ctx.clock.Now();
        const std::uint64_t elapsed = ElapsedMilliseconds(previous_time, now);
        previous_time = now;
        flash_bytes_index += kBeefFlashBufferSize;
        const TransferRate rate =
            ComputeTransferRate(kBeefFlashBufferSize, elapsed, flash_bytes_count - flash_bytes_index);
        LogInfo(ctx, FormatWriteProgress(address, percent, rate));

        progress.Update(static_cast<int>(std::min(flash_bytes_index, flash_bytes_count)));

        if ((offset + kBeefFlashBufferSize) % kBeefFlashCommitSize == 0)
        {
            const auto window_offset = offset + kBeefFlashBufferSize - kBeefFlashCommitSize;
            const Status committed =
                CommitBuffer(ctx, image.subspan(image_offset + window_offset, kBeefFlashCommitSize),
                             block.start + static_cast<std::uint32_t>(window_offset), test_write);
            if (!committed.has_value())
            {
                return committed;
            }
        }
    }
    return {};
}

} // namespace

BeefCanKernel::BeefCanKernel(ICanFlashTransport& transport, IClock& clock, const ICancellationToken& cancellation,
                             IEventSink& events, uds::IUdsChannel& channel, const DensoBeefCanProfile& profile)
    : transport_(transport), clock_(clock), cancellation_(cancellation), events_(events), channel_(channel),
      profile_(profile), uds_(channel, clock, events)
{
}

Status BeefCanKernel::TransferPayload(const KernelImage& kernel, const BeefKernelTransferPolicy& policy)
{
    const Result<bytes::Bytes> encrypted = PreparePayload(kernel.bytes, profile_.family);
    if (!encrypted.has_value())
    {
        return std::unexpected(encrypted.error());
    }
    events_.Log(LogLevel::kDebug, std::format("Start address to upload kernel: 0x{:x}", kernel.load_address));
    events_.Log(LogLevel::kInfo, "Initialize kernel upload");
    const auto request =
        bytes::ComposeBe(bytes::Byte{0x34}, bytes::Byte{0x04}, bytes::Byte{0x33}, bytes::U24{kernel.load_address},
                         bytes::U24{static_cast<std::uint32_t>(encrypted->size())});
    const Result<bytes::Bytes> reply = uds_.Request(request, policy.download_policy, cancellation_);
    if (!reply.has_value())
    {
        if (profile_.pdu_format == BeefPduFormat::kDescribe)
        {
            LogDescribedFailure(events_, profile_, reply.error());
        }
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.empty() || payload.front() != 0x20)
    {
        const Error mismatch{ErrorKind::kBadResponse, "unexpected response during kernel download"};
        if (profile_.pdu_format == BeefPduFormat::kDescribe)
        {
            LogDescribedFailure(events_, profile_, mismatch);
        }
        else
        {
            events_.Log(LogLevel::kError,
                        std::format("Wrong response from {} during kernel download", profile_.device));
        }
        return std::unexpected(mismatch);
    }

    events_.Log(LogLevel::kInfo, "Uploading kernel, please wait...");
    // The <= boundary deliberately sends a final address-only B6 request.
    for (std::size_t offset = 0; offset <= encrypted->size(); offset += kUploadBlockSize)
    {
        if (cancellation_.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "kernel upload cancelled");
        }
        const bytes::ByteView chunk =
            bytes::ByteView{*encrypted}.subspan(offset, std::min(kUploadBlockSize, encrypted->size() - offset));
        if (const Status sent = channel_.Send(
                bytes::ComposeBe(bytes::Byte{0xB6},
                                 bytes::U24{kernel.load_address + static_cast<std::uint32_t>(offset)}, chunk),
                cancellation_);
            !sent.has_value())
        {
            return sent;
        }
        // Raw reads tolerate malformed, wrong-ID and stale adapter content.
        const Result<std::optional<bytes::Bytes>> ignored = transport_.Read(policy.block_reply_timeout, cancellation_);
        if (!ignored.has_value() &&
            (ignored.error().kind == ErrorKind::kCancelled || ignored.error().kind == ErrorKind::kDisconnected))
        {
            return std::unexpected(ignored.error());
        }
        if (cancellation_.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "kernel upload cancelled");
        }
    }
    return {};
}

Result<bytes::Bytes> BeefCanKernel::Read(const MemoryRegion& region, PhaseReporter& progress)
{
    KernelContext view{cancellation_, events_, clock_, transport_, channel_, profile_};
    bytes::Bytes rom;
    rom.reserve(region.length);
    events_.Log(LogLevel::kInfo, "Start reading ROM, please wait...");
    // Fixed 0x400-byte requests return raw payloads; only the requested tail is
    // appended. Sampling each page and flooring elapsed time preserves the
    // portable replacement for the legacy first page's unstarted timer.
    for (std::uint32_t offset = 0; offset < region.length; offset += kReadPageSize)
    {
        const auto started = clock_.Now();
        if (const Status checkpoint = CancelledIfRequested(view, "read cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const std::uint32_t address = region.start + offset;
        const auto request = bytes::ComposeBe(bytes::Byte{0x00}, bytes::U24{address}, std::uint16_t{kReadPageSize});
        Result<bytes::Bytes> reply = Exchange(view, bytes::Byte{0x03}, request, kReadPageSize, kReadTimeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const std::size_t length = std::min<std::size_t>(kReadPageSize, region.length - offset);
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(started, clock_.Now());
        const TransferRate rate = ComputeTransferRate(kReadPageSize, elapsed_ms, region.length - offset);
        events_.Log(LogLevel::kInfo, FormatReadProgress(address, kReadPageSize, rate));
        rom.insert(rom.end(), reply->begin(), reply->begin() + static_cast<std::ptrdiff_t>(length));
        progress.Update(static_cast<int>(std::min<std::uint64_t>(region.length, offset + kReadPageSize)));
    }
    events_.Log(LogLevel::kInfo, "ROM read ready");
    return rom;
}

Status BeefCanKernel::Write(const FlashPlan& plan, PhaseSequence& phases, PhaseReporter& compare_progress)
{
    KernelContext ctx{cancellation_, events_, clock_, transport_, channel_, profile_};
    const bytes::ByteView image = plan.ImageOrEmpty();
    const auto image_base = plan.TransferRegion().start;
    const auto regions = plan.EraseRegions();
    const bool test_write = plan.Operation() == FlashOperation::kTestWrite;
    const auto before = CompareBlocks(ctx, regions, image_base, image, &compare_progress, false);
    if (!before.has_value())
    {
        return std::unexpected(before.error());
    }
    compare_progress.Complete();
    if (before->empty())
    {
        LogInfo(ctx, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        phases.Start("Write", 0);
        return {};
    }

    const std::size_t flash_bytes_count = std::ranges::fold_left(
        *before, std::size_t{0}, [&](std::size_t total, std::size_t index) { return total + regions[index].length; });
    LogInfo(ctx, "--- Start writing ROM file to ECU flash memory ---");
    if (const Status initialized = InitializeFlash(ctx, test_write); !initialized.has_value())
    {
        return initialized;
    }

    PhaseReporter write_progress = phases.Start("Write", static_cast<int>(flash_bytes_count));
    std::size_t flash_bytes_index = 0;
    for (const auto index : *before)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "flash block loop cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const MemoryRegion& block = regions[index];
        LogInfo(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
        LogInfo(ctx, "Check flash voltage");
        Result<bytes::Bytes> voltage = Exchange(ctx, kBeefProgVolt, bytes::ByteView{}, 2, kBeefVoltageTimeout);
        if (!voltage.has_value())
        {
            LogInfo(ctx, std::format("Block {} reflash failed.", index));
            return std::unexpected(voltage.error());
        }
        LogInfo(ctx, std::format(": {}V", static_cast<double>(bytes::ReadU16Be(*voltage)) / 50.0));

        const Status flashed =
            FlashBlock(ctx, image, image_base, block, test_write, flash_bytes_index, flash_bytes_count, write_progress);
        if (!flashed.has_value())
        {
            LogError(ctx, "Reflash error! Do not panic, do not reset the ECU immediately. The kernel is most likely "
                          "still running and receiving commands!");
            LogInfo(ctx, std::format("Block {} reflash failed.", index));
            return flashed;
        }
        LogInfo(ctx, "Flash block ok");
        LogInfo(ctx, std::format("Block {} reflash complete.", index));
    }
    write_progress.Complete();

    return CompareBlocks(ctx, regions, image_base, image, nullptr, true)
        .transform(
            [&](const auto& after)
            {
                if (test_write)
                {
                    LogInfo(ctx, "*** Test write PASS, it's ok to perform actual write! ***");
                }
                else if (!after.empty())
                {
                    LogError(ctx, "*** ERROR IN FLASH PROCESS ***");
                    LogError(ctx, "Don't power off your ECU, kernel is still running and you can try flashing again!");
                }
            });
}

} // namespace fastecu::flash
