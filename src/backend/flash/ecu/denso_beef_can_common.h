#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

// Helpers shared by the Denso BEEF dialect CAN executors:
// SubaruDensoSh7058CanExecutor, SubaruDensoSh7058CanDieselExecutor and
// SubaruTcuDensoSh705xCanExecutor.
//
// Each family was ported standalone with duplication tolerated, so factoring
// happened only once all three and their independent characterization tests
// were visible. Comparing all 22 signatures common to the three, exactly these
// bodies were byte-identical. parse_beef matched petrol and diesel only and
// stayed family-local, because a two-way match is where the original port design
// says to stop. The TCU copy has since proved to differ from them in comments
// alone, so ParseBeef, BeefMessage, UppercaseHexCompact and the request half
// of the channel exchange (SendChannelRequest) are shared below.
//
// Everything substantial was compared and deliberately left family-local:
// connect_bootloader, flash_block, upload_kernel, write_memory,
// compare_blocks, read_memory, beef_exchange, upload_b6_discard,
// nonfatal_query, strict_payload, discard_stale_frame, query_crc and
// reflash_block all differ across all three in timeouts, retry counts,
// geometry, address indexing, startup ordering or log wording.
//
// Inside upload_kernel, two steps are identical in all three and are shared
// below: building the checksummed, encrypted payload, and walking it in 0xB6
// blocks. Each family keeps its own size checks, wording, B6 reply handling
// and kernel start.
//
// The executor suites do NOT read these helpers back: each carries wire
// expectations transcribed independently from its legacy oracle, so a wrong
// change here fails those suites rather than passing silently. Keep it that
// way.
namespace fastecu::flash
{

// The two-byte frame marker every BEEF-protocol request and reply opens with.
// Byte-identical across all three consumers.
inline constexpr std::uint32_t kKernelStartComm = 0xBEEF;

// Frames a kernel-protocol request: marker, big-endian length covering the
// opcode plus payload, opcode, payload.
inline bytes::Bytes BeefRequest(bytes::Byte opcode, bytes::ByteView payload = {})
{
    return bytes::ComposeBe(std::uint16_t{kKernelStartComm}, static_cast<std::uint16_t>(payload.size() + 1), opcode,
                            payload);
}

// The padded kernel-upload payload transform. A one-line adapter onto the
// shared table in denso_iso15765_can_common.h.
inline bytes::Bytes EncryptPayload(bytes::ByteView payload)
{
    return DensoEncryptRom(payload);
}

// The kernel zero-padded to whole `block_size` transfer blocks, its last word
// replaced by the 0x5AA5A55A complement of the other words' 32-bit sum, then
// encrypted. Empty when the kernel is empty or its padding overflows.
// `block_size` is a nonzero multiple of four.
inline std::optional<bytes::Bytes> BeefKernelPayload(bytes::ByteView kernel, std::size_t block_size)
{
    const std::size_t padded_to_word = (kernel.size() + 3U) & ~std::size_t{3U};
    const std::size_t block_count = (padded_to_word + block_size - 1U) / block_size;
    if (block_count == 0 || block_count > std::numeric_limits<std::size_t>::max() / block_size)
    {
        return std::nullopt;
    }
    bytes::Bytes plain(kernel.begin(), kernel.end());
    plain.resize(block_count * block_size - 4U, bytes::Byte{0});
    std::uint32_t sum = 0;
    for (std::size_t offset = 0; offset < plain.size(); offset += 4)
    {
        sum += bytes::ReadU32Be(plain, offset);
    }
    bytes::AppendU32Be(plain, 0x5AA5A55AU - sum);
    return EncryptPayload(plain);
}

// A parsed BEEF reply: its opcode and the payload the declared length covers.
struct BeefMessage
{
    bytes::Byte opcode;
    bytes::ByteView payload;
};

inline Result<BeefMessage> ParseBeef(bytes::ByteView pdu)
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

// Upper-case hex of `data` with no separators.
inline std::string UppercaseHexCompact(bytes::ByteView data)
{
    std::string rendered;
    for (const bytes::Byte value : data)
    {
        std::format_to(std::back_inserter(rendered), "{:02X}", value);
    }
    return rendered;
}

// Any executor context carrying a cancellation token.
template <typename C>
concept WithCancellation = requires(const C& ctx) {
    { ctx.cancellation } -> std::convertible_to<const ICancellationToken&>;
};

template <WithCancellation C> Status CancelledIfRequested(const C& ctx, std::string_view detail)
{
    if (ctx.cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::string(detail));
    }
    return {};
}

// Legacy reported a floor of one millisecond for any completed transfer, so a
// sub-millisecond phase never renders as "0 ms".
inline std::uint64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start,
                                         std::chrono::steady_clock::time_point end)
{
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 1U;
}

// The request half of every BEEF exchange: cancellation check, send on the
// context's channel, then an optional post-send settle delay. Templated because
// each family's channel member has its own type.
template <typename C> Status SendChannelRequest(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds delay)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "cancelled before CAN request"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    if (delay > std::chrono::milliseconds{0})
    {
        return ctx.clock.Sleep(delay, ctx.cancellation);
    }
    return {};
}

// Sends `pdu` and returns whatever arrives within `timeout`.
template <typename C>
Result<std::optional<bytes::Bytes>>
ChannelRequestOptional(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    if (const Status sent = SendChannelRequest(ctx, pdu, delay); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    return ctx.channel.Receive(timeout, ctx.cancellation);
}

// The short raw read that follows a CRC reply. The legacy operation does not
// validate it, so malformed and wrong-ID frames stay tolerated stale content;
// only cancellation and disconnection are actionable transport failures.
// 200 ms is every consumer's kShortTimeout.
template <typename C> Status DiscardStaleFrame(C& ctx)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "CRC stale-frame drain cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    Result<std::optional<bytes::Bytes>> stale = ctx.transport.Read(std::chrono::milliseconds{200}, ctx.cancellation);
    if (!stale.has_value() &&
        (stale.error().kind == ErrorKind::kCancelled || stale.error().kind == ErrorKind::kDisconnected))
    {
        return std::unexpected(stale.error());
    }
    return {};
}

// The CRC query every BEEF executor issues for a block: a start/length
// request, whose four-byte reply is the CRC, then the stale-frame drain.
// `exchange` sends the payload with the family's opcode and timeout.
template <typename C, typename Exchange>
Result<std::uint32_t> QueryBeefCrc(C& ctx, const MemoryRegion& block, Exchange exchange)
{
    Result<bytes::Bytes> reply = exchange(ComposeBe(block.start, bytes::Byte{0x00}, bytes::U24(block.length)));
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const std::uint32_t crc = bytes::ReadU32Be(*reply);
    if (const Status drained = DiscardStaleFrame(ctx); !drained.has_value())
    {
        return std::unexpected(drained.error());
    }
    return crc;
}

// Which erase regions the ECU's CRC disagrees with the image on.
struct CompareResult
{
    std::vector<bool> modified;
    std::size_t changed_count{};
};

// write_mem()/get_changed_blocks(): asks the ECU for the CRC of every erase
// region and flags those that differ from the image. `query_crc` fetches one
// region's CRC; `family` names the executor in the out-of-image error.
template <typename C, typename QueryCrc>
Result<CompareResult> CompareBeefBlocks(C& ctx, const FlashPlan& plan, bytes::ByteView image, PhaseReporter *progress,
                                        bool after_reflash, std::string_view family, QueryCrc query_crc)
{
    CompareResult result;
    result.modified.assign(plan.EraseRegions().size(), false);
    LogInfo(ctx, after_reflash ? "--- Comparing ECU flash memory pages to image file after reflash ---"
                               : "--- Comparing ECU flash memory pages to image file ---");
    LogInfo(ctx, "blk\tstart\tlen\tecu crc\timg crc\tsame?");
    for (std::size_t index = 0; index < plan.EraseRegions().size(); ++index)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "ROM compare cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const MemoryRegion block = plan.EraseRegions()[index];
        if (block.start < plan.TransferRegion().start ||
            static_cast<std::uint64_t>(block.start - plan.TransferRegion().start) + block.length > image.size())
        {
            return Fail(ErrorKind::kInvalidConfig, std::format("{} CRC block is outside the image", family));
        }
        LogInfo(ctx, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        Result<std::uint32_t> ecu_crc = query_crc(block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::size_t image_offset = block.start - plan.TransferRegion().start;
        const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(image_offset, block.length));
        LogDebug(ctx, std::format("ROM CRC: 0x{:08x} IMG CRC: 0x{:08x}", *ecu_crc, image_crc));
        LogInfo(ctx, std::format("\t{:08X}\t{:08X}", *ecu_crc, image_crc));
        result.modified[index] = *ecu_crc != image_crc;
        LogInfo(ctx, result.modified[index] ? "\tNO" : "\tYES");
        if (result.modified[index])
        {
            ++result.changed_count;
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
    for (std::size_t index = 0; index < result.modified.size(); ++index)
    {
        if (result.modified[index])
        {
            LogInfo(ctx, std::format("{}, ", index));
        }
    }
    LogInfo(ctx, std::format(" (total: {})", result.changed_count));
    return result;
}

// Sends `payload` to `load_address` in `block_size` 0xB6 blocks, then one
// final address-only block, as every BEEF oracle does. `upload` sends one
// request and handles its reply the family's way.
template <WithCancellation C, typename Upload>
Status UploadBeefKernelBlocks(const C& ctx, bytes::ByteView payload, std::uint32_t load_address, std::size_t block_size,
                              Upload upload)
{
    for (std::size_t offset = 0; offset <= payload.size(); offset += block_size)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "kernel upload cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const bytes::ByteView chunk = payload.subspan(offset, std::min(block_size, payload.size() - offset));
        if (const Status sent = upload(bytes::ComposeBe(
                bytes::Byte{0xB6}, bytes::U24(load_address + static_cast<std::uint32_t>(offset)), chunk));
            !sent.has_value())
        {
            return sent;
        }
    }
    return {};
}

} // namespace fastecu::flash
