#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/backend/flash/ecu/denso_iso15765_can_common.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

// Helpers shared by the wave-5 Denso BEEF-protocol CAN executors:
// SubaruDensoSh7058CanExecutor, SubaruDensoSh7058CanDieselExecutor and
// SubaruTcuDensoSh705xCanExecutor.
//
// Each family was ported standalone with duplication tolerated, so factoring
// happened only once all three and their independent characterization tests
// were visible. Comparing all 22 signatures common to the three, exactly these
// bodies were byte-identical. parse_beef matched petrol and diesel only and
// stays family-local, because a two-way match is where this wave's design
// says to stop.
//
// Everything substantial was compared and deliberately left family-local:
// connect_bootloader, flash_block, upload_kernel, write_memory,
// compare_blocks, read_memory, beef_exchange, upload_b6_discard,
// nonfatal_query, strict_payload, discard_stale_frame, query_crc and
// reflash_block all differ across all three in timeouts, retry counts,
// geometry, address indexing, startup ordering or log wording.
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
inline bytes::Bytes beef_request(bytes::Byte opcode, bytes::ByteView payload = {})
{
    return bytes::composeBe(std::uint16_t{kKernelStartComm}, static_cast<std::uint16_t>(payload.size() + 1), opcode,
                            payload);
}

// The padded kernel-upload payload transform. A one-line adapter onto the
// shared table in denso_iso15765_can_common.h.
inline bytes::Bytes encrypt_payload(bytes::ByteView payload)
{
    return denso_encrypt_rom(payload);
}

// Any executor context carrying a cancellation token.
template <class C>
concept WithCancellation = requires(const C& ctx) {
    { ctx.cancellation } -> std::convertible_to<const ICancellationToken&>;
};

template <WithCancellation C> Status cancelled_if_requested(const C& ctx, std::string_view detail)
{
    if (ctx.cancellation.cancelled())
    {
        return fail(ErrorKind::Cancelled, std::string(detail));
    }
    return {};
}

// Legacy reported a floor of one millisecond for any completed transfer, so a
// sub-millisecond phase never renders as "0 ms".
inline std::uint64_t elapsed_milliseconds(std::chrono::steady_clock::time_point start,
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
channel_request_optional(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                         std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    if (const Status checkpoint = cancelled_if_requested(ctx, "cancelled before CAN request"); !checkpoint.has_value())
    {
        return std::unexpected(checkpoint.error());
    }
    if (const Status sent = ctx.channel.send(pdu, ctx.cancellation); !sent.has_value())
    {
        return std::unexpected(sent.error());
    }
    if (delay > std::chrono::milliseconds{0})
    {
        if (const Status slept = ctx.clock.sleep(delay, ctx.cancellation); !slept.has_value())
        {
            return std::unexpected(slept.error());
        }
    }
    return ctx.channel.receive(timeout, ctx.cancellation);
}

} // namespace fastecu::flash
