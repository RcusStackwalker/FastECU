#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_response.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"
#include "src/backend/flash/ecu/beef_compare_result.h"
#include "src/backend/flash/ecu/denso_iso15765_crypto.h"
#include "src/backend/flash/ecu/flash_phase_progress.h"
#include "src/backend/flash/ecu/uds_client_exchange_common.h"
#include "src/backend/flash/transfer_progress.h"
#include "src/backend/flash/flash_types.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/error.h"
#include "src/backend/ports/result.h"

// Internals of the Denso BEEF dialect CAN executors: SubaruDensoSh7058CanExecutor,
// SubaruDensoSh7058CanDieselExecutor and SubaruTcuDensoSh705xCanExecutor.
//
// BeefCanSession owns the conversation these helpers implement: the tolerant
// query, identity, CRC compare, flash init and write. It takes the family's
// wording from a DensoBeefCanProfile and calls the templates below, so callers
// of those flows use the session, not these functions.
//
// Executors still call this header directly for what stays family-local: the
// channel request helpers (SendChannelRequest, ChannelRequest,
// ChannelRequestOptional), CancelledIfRequested and ElapsedMilliseconds,
// ParseBeef and BeefRequest for kernel-ID probing, the 0xB6 kernel-upload
// helpers (BeefKernelPayload, UploadBeefKernelBlocks, DiscardB6Reply), and
// ReadBeefMemory for the petrol and diesel reads. Connect, the TCU's read and
// flash-block loops, security and kernel upload differ per family in
// timeouts, retry counts, geometry and log wording, and stay in each executor.
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

// The initial upload request describes the encrypted payload without embedding
// it. Callers retain their existing kernel-size validation.
inline std::array<bytes::Byte, 9> BeefKernelDownloadRequest(std::uint32_t load_address,
                                                            bytes::ByteView encrypted_payload)
{
    return bytes::ComposeBe(bytes::Byte{0x34}, bytes::Byte{0x04}, bytes::Byte{0x33}, bytes::U24{load_address},
                            bytes::U24{static_cast<std::uint32_t>(encrypted_payload.size())});
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

// BeefKernelPayload for a family that sends the length in a 24-bit wire field;
// `family` names the executor in the errors.
inline Result<bytes::Bytes> BeefKernelUploadPayload(bytes::ByteView kernel, std::size_t block_size,
                                                    std::string_view family)
{
    std::optional<bytes::Bytes> encrypted = BeefKernelPayload(kernel, block_size);
    if (!encrypted.has_value())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload size is invalid", family));
    }
    if (encrypted->size() >= 0x1000000U)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} kernel upload exceeds the 24-bit wire length", family));
    }
    return std::move(*encrypted);
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

// ChannelRequestOptional where silence is a timeout error.
template <typename C>
Result<bytes::Bytes> ChannelRequest(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds timeout,
                                    std::chrono::milliseconds delay = std::chrono::milliseconds{0})
{
    Result<std::optional<bytes::Bytes>> received = ChannelRequestOptional(ctx, pdu, timeout, delay);
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

// A BEEF request whose reply must carry `opcode | 0x40` and at least
// `minimum_payload` payload bytes; returns that payload.
template <typename C>
Result<bytes::Bytes> BeefExchange(C& ctx, bytes::Byte opcode, bytes::ByteView payload, std::size_t minimum_payload,
                                  std::chrono::milliseconds timeout)
{
    Result<bytes::Bytes> reply = ChannelRequest(ctx, BeefRequest(opcode, payload), timeout);
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

// Requests a programming session in mode 0x02 for an accepted session 0x03
// and mode 0x42 for an accepted session 0x43, and accepts a reply echoing
// either mode.
template <typename C>
Status EnterBeefProgrammingSession(C& ctx, bool session_03_accepted, bool session_43_accepted,
                                   const uds::ExchangePolicy& policy)
{
    LogInfo(ctx, "Set session mode");
    bytes::Bytes request{uds::kSidDiagnosticSessionControl};
    if (session_03_accepted)
    {
        request.push_back(0x02);
    }
    if (session_43_accepted)
    {
        request.push_back(0x42);
    }
    Result<bytes::Bytes> reply = ctx.uds.Request(request, policy, ctx.cancellation);
    if (!reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    const bytes::ByteView payload = uds::Payload(*reply);
    if (payload.empty() || (payload[0] != 0x02 && payload[0] != 0x42))
    {
        return Fail(ErrorKind::kBadResponse, "unexpected programming session response");
    }
    LogInfo(ctx, "Succesfully set to programming session");
    return {};
}

// The petrol and diesel connect tail: asks the session which bootloader
// sessions the ECU accepts, unlocks with the family's `unlock(ctx)`, then
// enters the programming session those sessions select.
template <typename C, typename Unlock>
Status ConnectBeefProgrammingSession(C& ctx, Unlock unlock, const uds::ExchangePolicy& policy)
{
    auto sessions = ctx.session.RequestBootloaderSessions();
    if (!sessions.has_value())
    {
        return std::unexpected(sessions.error());
    }
    if (Status unlocked = unlock(ctx); !unlocked.has_value())
    {
        return unlocked;
    }
    return EnterBeefProgrammingSession(ctx, sessions->mode_03, sessions->mode_43, policy);
}

// Every 0xB6 kernel-upload block is followed by a raw read whose content is
// ignored. Send through the channel for the request CAN envelope, but read the
// reply directly so short, malformed and wrong-ID frames and adapter stale
// errors stay non-fatal; only cancellation and disconnection are typed.
template <typename C> Status DiscardB6Reply(C& ctx, bytes::ByteView pdu, std::chrono::milliseconds read_timeout)
{
    if (const Status checkpoint = CancelledIfRequested(ctx, "kernel upload cancelled"); !checkpoint.has_value())
    {
        return checkpoint;
    }
    if (const Status sent = ctx.channel.Send(pdu, ctx.cancellation); !sent.has_value())
    {
        return sent;
    }
    const Result<std::optional<bytes::Bytes>> ignored = ctx.transport.Read(read_timeout, ctx.cancellation);
    if (!ignored.has_value() &&
        (ignored.error().kind == ErrorKind::kCancelled || ignored.error().kind == ErrorKind::kDisconnected))
    {
        return std::unexpected(ignored.error());
    }
    return CancelledIfRequested(ctx, "kernel upload cancelled");
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
    Result<bytes::Bytes> reply = exchange(ComposeBe(block.start, bytes::Byte{0x00}, bytes::U24{block.length}));
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

// write_mem()/get_changed_blocks(): asks the ECU for the CRC of every erase
// region and flags those that differ from the image. `query_crc` fetches one
// region's CRC; `image_base` is the ECU address of the image's first byte;
// `family` names the executor in the out-of-image error.
template <typename C, typename QueryCrc>
Result<CompareResult> CompareBeefBlocks(C& ctx, std::span<const MemoryRegion> blocks, std::uint32_t image_base,
                                        bytes::ByteView image, PhaseReporter *progress, bool after_reflash,
                                        std::string_view family, QueryCrc query_crc)
{
    CompareResult result;
    result.modified.assign(blocks.size(), false);
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
            return Fail(ErrorKind::kInvalidConfig, std::format("{} CRC block is outside the image", family));
        }
        LogInfo(ctx, std::format("FB{:02}\t0x{:08X}\t0x{:08X}", index, block.start, block.length));
        Result<std::uint32_t> ecu_crc = query_crc(block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::size_t image_offset = block.start - image_base;
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

// init_flash_write(): reads the kernel's maximum message and flash block sizes,
// then selects test-write or real-write mode. `exchange` sends one opcode and
// returns its reply once it holds the given number of payload bytes. Logs the
// sizes in upper-case hex when `upper_hex`, lower-case otherwise.
template <typename C, typename Exchange>
Status InitializeBeefFlash(const C& ctx, bool test_write, bool upper_hex, Exchange exchange)
{
    const auto log_size = [&](const bytes::Bytes& reply)
    {
        const std::uint32_t size = bytes::ReadU32Be(reply);
        LogInfo(ctx, upper_hex ? std::format(": 0x{:04X}", size) : std::format(": 0x{:04x}", size));
    };
    LogInfo(ctx, "Check max message length");
    Result<bytes::Bytes> max_message = exchange(bytes::Byte{0x05}, 4);
    if (!max_message.has_value())
    {
        return std::unexpected(max_message.error());
    }
    log_size(*max_message);

    LogInfo(ctx, "Check flashblock size");
    Result<bytes::Bytes> max_block = exchange(bytes::Byte{0x06}, 4);
    if (!max_block.has_value())
    {
        return std::unexpected(max_block.error());
    }
    log_size(*max_block);

    LogInfo(ctx, test_write ? "Test write mode on, no actual flash write is performed"
                            : "Test write mode off, perform actual flash write");
    Result<bytes::Bytes> mode = exchange(bytes::Byte{test_write ? std::uint8_t{0x21} : std::uint8_t{0x20}}, 0);
    if (!mode.has_value())
    {
        return std::unexpected(mode.error());
    }
    LogError(ctx, "Flash mode succesfully set");
    return {};
}

// read_mem(): reads `region` in 0x400-byte pages of proprietary BEEF replies,
// appended as received (the legacy read path performs no payload decryption).
// The legacy samples an unstarted timer for the first page's rate; a
// monotonic sample floored to 1 ms stands in for it.
inline constexpr std::size_t kBeefReadPageSize = 0x400;
inline constexpr std::chrono::milliseconds kBeefReadTimeout{2000};

template <typename C, typename Exchange>
Result<bytes::Bytes> ReadBeefMemory(C& ctx, const MemoryRegion& region, PhaseReporter& progress, Exchange exchange)
{
    bytes::Bytes rom;
    rom.reserve(region.length);
    LogInfo(ctx, "Start reading ROM, please wait...");
    for (std::uint32_t offset = 0; offset < region.length; offset += kBeefReadPageSize)
    {
        const auto started = ctx.clock.Now();
        if (const Status checkpoint = CancelledIfRequested(ctx, "read cancelled"); !checkpoint.has_value())
        {
            return std::unexpected(checkpoint.error());
        }
        const std::uint32_t address = region.start + offset;
        const auto request = bytes::ComposeBe(bytes::Byte{0x00}, bytes::U24{address}, std::uint16_t{kBeefReadPageSize});
        Result<bytes::Bytes> reply = exchange(bytes::Byte{0x03}, request, kBeefReadPageSize, kBeefReadTimeout);
        if (!reply.has_value())
        {
            return std::unexpected(reply.error());
        }
        const std::size_t length = std::min<std::size_t>(kBeefReadPageSize, region.length - offset);
        const std::uint64_t elapsed_ms = ElapsedMilliseconds(started, ctx.clock.Now());
        const TransferRate rate = ComputeTransferRate(kBeefReadPageSize, elapsed_ms, region.length - offset);
        LogInfo(ctx, FormatReadProgress(address, kBeefReadPageSize, rate));
        rom.insert(rom.end(), reply->begin(), reply->begin() + static_cast<std::ptrdiff_t>(length));
        progress.Update(static_cast<int>(std::min<std::uint64_t>(region.length, offset + kBeefReadPageSize)));
    }
    LogInfo(ctx, "ROM read ready");
    return rom;
}

// The write path shared by the petrol and diesel SH7058 executors, whose
// flash_block()/reflash_block()/write_mem() ports are identical but for the
// family named in their errors. The kernel opcodes, buffer windows and
// timeouts below are those of the legacy oracle.
inline constexpr std::size_t kBeefFlashBufferSize = 0x200;
inline constexpr std::size_t kBeefFlashCommitSize = 0x1000;
inline constexpr bytes::Byte kBeefProgVolt = 0x04;
inline constexpr bytes::Byte kBeefWriteBuffer = 0x22;
inline constexpr bytes::Byte kBeefValidateBuffer = 0x23;
inline constexpr bytes::Byte kBeefCommitBuffer = 0x24;
inline constexpr bytes::Byte kBeefBlankPage = 0x25;
inline constexpr std::chrono::milliseconds kBeefWriteTimeout{800};
inline constexpr std::chrono::milliseconds kBeefVoltageTimeout{500};
inline constexpr std::chrono::milliseconds kBeefEraseTimeout{3000};

// Erases `block`, writes it in 0x200-byte buffers and, every 0x1000 bytes,
// commits (or, for a test write, validates) the window with its CRC. `exchange`
// is the family's BeefExchange; `image_base` is the ECU address of the image's
// first byte. Sampling the clock first and flooring a 0 ms interval to 1
// avoids the legacy's uninitialized speed and time-left values.
template <typename C, typename Exchange>
Status FlashBeefBlock(C& ctx, bytes::ByteView image, std::uint32_t image_base, const MemoryRegion& block,
                      bool test_write, std::size_t& flash_bytes_index, std::size_t flash_bytes_count,
                      PhaseReporter& progress, std::string_view family, Exchange exchange)
{
    if (block.length == 0 || block.length % kBeefFlashBufferSize != 0 || block.length % kBeefFlashCommitSize != 0)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} flash block is not aligned to write windows", family));
    }
    if (block.start < image_base)
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} flash block starts before the image", family));
    }
    const std::size_t image_offset = block.start - image_base;
    if (image_offset + block.length > image.size())
    {
        return Fail(ErrorKind::kInvalidConfig, std::format("{} flash block extends beyond the image", family));
    }

    LogInfo(ctx, std::format("Flash page erase addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    LogInfo(ctx, "Erasing flash page...");
    Result<bytes::Bytes> erased = exchange(kBeefBlankPage, bytes::ComposeBe(block.start), 0, kBeefEraseTimeout);
    if (!erased.has_value())
    {
        return std::unexpected(erased.error());
    }
    LogInfo(ctx, " erased");

    auto previous_time = ctx.clock.Now();
    std::uint32_t address = block.start;
    std::uint32_t remaining = block.length;
    std::uint32_t commit_start = block.start;
    LogInfo(ctx, std::format("Start flash write addr: 0x{:08x} len: 0x{:08x}", block.start, block.length));
    while (remaining != 0)
    {
        if (const Status checkpoint = CancelledIfRequested(ctx, "flash write cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const std::size_t chunk_offset = static_cast<std::size_t>(address - block.start);
        const bytes::ByteView chunk = image.subspan(image_offset + chunk_offset, kBeefFlashBufferSize);
        Result<bytes::Bytes> written =
            exchange(kBeefWriteBuffer, bytes::ComposeBe(address, chunk), 0, kBeefWriteTimeout);
        if (!written.has_value())
        {
            return std::unexpected(written.error());
        }
        LogDebug(ctx, "Data written to flash buffer");

        const std::uint32_t percent = static_cast<std::uint32_t>(100U * (block.length - remaining) / block.length);
        const auto now = ctx.clock.Now();
        const std::uint64_t elapsed = ElapsedMilliseconds(previous_time, now);
        previous_time = now;
        const auto next_index = flash_bytes_index + kBeefFlashBufferSize;
        const TransferRate rate = ComputeTransferRate(kBeefFlashBufferSize, elapsed, flash_bytes_count - next_index);
        LogInfo(ctx, FormatWriteProgress(address, percent, rate));

        remaining -= static_cast<std::uint32_t>(kBeefFlashBufferSize);
        address += kBeefFlashBufferSize;
        flash_bytes_index += kBeefFlashBufferSize;
        progress.Update(static_cast<int>(std::min(flash_bytes_index, flash_bytes_count)));

        if (address - commit_start == kBeefFlashCommitSize)
        {
            if (const Status checkpoint = CancelledIfRequested(ctx, "flash commit cancelled"); !checkpoint.has_value())
            {
                return checkpoint;
            }
            LogInfo(ctx, "Flash buffer write complete... ");
            const std::size_t crc_offset = image_offset + static_cast<std::size_t>(commit_start - block.start);
            const std::uint32_t image_crc = fastecu::checksum::Crc32(image.subspan(crc_offset, kBeefFlashCommitSize));
            LogDebug(ctx, std::format("Image CRC32: 0x{:x}", image_crc));
            LogInfo(ctx, std::format("{} flash addr: 0x{:x}", test_write ? "Validate" : "Committ", commit_start));
            LogInfo(ctx, std::format(" len: 0x{:x}", kBeefFlashCommitSize));
            LogInfo(ctx, std::format(" crc32: 0x{:x}", image_crc));
            const auto commit = bytes::ComposeBe(commit_start, std::uint16_t{kBeefFlashCommitSize}, image_crc);
            Result<bytes::Bytes> committed =
                exchange(test_write ? kBeefValidateBuffer : kBeefCommitBuffer, commit, 0, kBeefEraseTimeout);
            if (!committed.has_value())
            {
                return std::unexpected(committed.error());
            }
            commit_start += kBeefFlashCommitSize;
        }
    }
    return {};
}

// write_mem(): compares the ECU with the image, then reflashes every
// differing erase region (checking the programming voltage first) and
// compares again. `compare(progress, after_reflash)` and `initialize(test_write)`
// are the family's CompareBlocks and InitializeFlash.
template <typename C, typename Exchange, typename Compare, typename Initialize>
Status WriteBeefMemory(C& ctx, bytes::ByteView image, std::uint32_t image_base, std::span<const MemoryRegion> regions,
                       bool test_write, PhaseSequence& phases, PhaseReporter& compare_progress, std::string_view family,
                       Exchange exchange, Compare compare, Initialize initialize)
{
    Result<CompareResult> before = compare(&compare_progress, false);
    if (!before.has_value())
    {
        return std::unexpected(before.error());
    }
    compare_progress.Complete();
    if (before->changed_count == 0)
    {
        LogInfo(ctx, "*** Compare results no difference between ROM and ECU data, no flashing needed! ***");
        phases.Start("Write", 0);
        return {};
    }

    std::size_t flash_bytes_count = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (before->modified[index])
        {
            flash_bytes_count += regions[index].length;
        }
    }
    LogInfo(ctx, "--- Start writing ROM file to ECU flash memory ---");
    if (const Status initialized = initialize(test_write); !initialized.has_value())
    {
        return initialized;
    }

    PhaseReporter write_progress = phases.Start("Write", static_cast<int>(flash_bytes_count));
    std::size_t flash_bytes_index = 0;
    for (std::size_t index = 0; index < before->modified.size(); ++index)
    {
        if (!before->modified[index])
        {
            continue;
        }
        if (const Status checkpoint = CancelledIfRequested(ctx, "flash block loop cancelled"); !checkpoint.has_value())
        {
            return checkpoint;
        }
        const MemoryRegion& block = regions[index];
        LogInfo(ctx, std::format("Flash block addr: 0x{:08X} len: 0x{:08X}", block.start, block.length));
        LogInfo(ctx, "Check flash voltage");
        Result<bytes::Bytes> voltage = exchange(kBeefProgVolt, bytes::ByteView{}, 2, kBeefVoltageTimeout);
        if (!voltage.has_value())
        {
            LogInfo(ctx, std::format("Block {} reflash failed.", index));
            return std::unexpected(voltage.error());
        }
        LogInfo(ctx, std::format(": {}V", static_cast<double>(bytes::ReadU16Be(*voltage)) / 50.0));

        const Status flashed = FlashBeefBlock(ctx, image, image_base, block, test_write, flash_bytes_index,
                                              flash_bytes_count, write_progress, family, exchange);
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

    Result<CompareResult> after = compare(nullptr, true);
    if (!after.has_value())
    {
        return std::unexpected(after.error());
    }
    if (test_write)
    {
        LogInfo(ctx, "*** Test write PASS, it's ok to perform actual write! ***");
    }
    else if (after->changed_count != 0)
    {
        LogError(ctx, "*** ERROR IN FLASH PROCESS ***");
        LogError(ctx, "Don't power off your ECU, kernel is still running and you can try flashing again!");
    }
    return {};
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
                bytes::Byte{0xB6}, bytes::U24{load_address + static_cast<std::uint32_t>(offset)}, chunk));
            !sent.has_value())
        {
            return sent;
        }
    }
    return {};
}

} // namespace fastecu::flash
