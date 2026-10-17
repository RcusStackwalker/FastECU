#pragma once

#include <algorithm>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
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
// stays family-local, because a two-way match is where the original port design
// says to stop.
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

// Any executor context carrying a cancellation token.
template <class C>
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

// Sends `pdu` on the context's channel and returns whatever arrives within
// `timeout`, with an optional post-send settle delay. Templated because each
// family's channel member has its own type.
template <class C>
Result<std::optional<bytes::Bytes>>
ChannelRequestOptional(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                       std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "cancelled before CAN request"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay > std::chrono::milliseconds{0})
    {
        if (const Status slept = ctx.clock.Sleep(delay, ctx.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return ctx.channel.Receive(timeout, ctx.cancellation);
}

// Sends `payload` to `load_address` in `block_size` 0xB6 blocks, then one
// final address-only block, as every BEEF oracle does. `upload` sends one
// request and handles its reply the family's way.
template <WithCancellation C, class Upload>
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
