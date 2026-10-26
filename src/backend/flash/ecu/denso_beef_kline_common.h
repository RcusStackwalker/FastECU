#pragma once

#include <chrono>
#include <cstddef>
#include <format>
#include <span>
#include <string_view>
#include <vector>
#include "src/algorithms/checksum/checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{
// The BEEF K-Line dialect is distinct from the BEEF CAN request format.
inline constexpr std::uint16_t kDensoBeefKlineStartComm = 0xBEEF;
inline constexpr bytes::Byte kDensoKlineOpId = 0x01;
inline constexpr bytes::Byte kDensoKlineOpCrc = 0x02;
inline constexpr bytes::Byte kDensoKlineOpReadArea = 0x03;
inline constexpr bytes::Byte kDensoKlineOpProgVolt = 0x04;
inline constexpr bytes::Byte kDensoKlineOpGetMaxMsgSize = 0x05;
inline constexpr bytes::Byte kDensoKlineOpGetMaxBlockSize = 0x06;
inline constexpr bytes::Byte kDensoKlineOpFlashEnable = 0x20;
inline constexpr bytes::Byte kDensoKlineOpFlashDisable = 0x21;
inline constexpr bytes::Byte kDensoKlineOpWriteFlashBuffer = 0x22;
inline constexpr bytes::Byte kDensoKlineOpValidateFlashBuffer = 0x23;
inline constexpr bytes::Byte kDensoKlineOpCommitFlashBuffer = 0x24;
inline constexpr bytes::Byte kDensoKlineOpBlankPage = 0x25;
inline constexpr bytes::Byte kDensoKlineOpUploadKernel = 0x53;

// Marker, 16-bit length of opcode plus payload, opcode, payload and sum8.
// Length conversion intentionally retains the original modulo-65536 behavior.
bytes::Bytes DensoBeefKlineFrame(bytes::Byte opcode, bytes::ByteView payload = {});
// Deliberately permissive: marker, six-byte minimum and ACK only.
bool DensoBeefKlineResponseOk(bytes::ByteView response, bytes::Byte ack);
Result<IKlineFlashTransport::OptionalBytes>
DensoBeefKlineExchangeOptional(IKlineFlashTransport& transport, IClock *clock, const ICancellationToken& cancellation,
                               bytes::ByteView request, std::chrono::milliseconds settle,
                               std::chrono::milliseconds timeout);
bytes::Bytes DensoBeefKlineWritePayload(std::uint32_t address, bytes::ByteView chunk);
Status DensoBeefKlineEnableProgrammingVoltage(IKlineFlashTransport& transport, const ICancellationToken& cancellation);
// Writes all of `request`; a short write means the adapter went away.
Status DensoBeefKlineWriteAll(IKlineFlashTransport& transport, bytes::ByteView request);
// Accepts a reply longer than `more_than` bytes that acknowledges `opcode`.
Status DensoBeefKlineCheckAck(bytes::ByteView response, bytes::Byte opcode, std::string_view phase,
                              std::size_t more_than = 0);
// Exchanges `request` and requires an acknowledging reply; none times out.
Result<bytes::Bytes> DensoBeefKlineExchangeAck(IKlineFlashTransport& transport, IClock *clock,
                                               const ICancellationToken& cancellation, bytes::ByteView request,
                                               std::chrono::milliseconds settle, std::chrono::milliseconds timeout,
                                               bytes::Byte opcode, std::string_view phase, std::size_t more_than = 0);

// Requests the ECU's CRC over `block` and returns the first chunk of its reply
// (empty when none arrives within 3 s); each family reads the rest and
// decodes it under its own acceptance rules.
Result<bytes::Bytes> DensoBeefKlineRequestBlockCrc(IKlineFlashTransport& transport,
                                                   const ICancellationToken& cancellation, const MemoryRegion& block);

// Runs `action` between cancellation checks reading "cancelled before <what>"
// and "cancelled after <what>".
template <typename Action>
Status DensoBeefKlineGuarded(const ICancellationToken& cancellation, std::string_view what, Action&& action)
{
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::format("cancelled before {}", what));
    }
    if (Status done = action(); !done.has_value())
    {
        return done;
    }
    if (cancellation.Cancelled())
    {
        return Fail(ErrorKind::kCancelled, std::format("cancelled after {}", what));
    }
    return {};
}

// Callers provide valid absolute-addressed image slices and one flag per block.
template <typename ReadCrc>
Result<unsigned> DensoBeefKlineCompareBlocks(std::span<const MemoryRegion> blocks, bytes::ByteView addressed_image,
                                             std::vector<bool>& modified, const ICancellationToken& cancellation,
                                             ReadCrc&& read_crc)
{
    unsigned modified_count = 0;
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        if (cancellation.Cancelled())
        {
            return Fail(ErrorKind::kCancelled, "cancelled during ROM compare");
        }
        const MemoryRegion& block = blocks[index];
        const Result<std::uint32_t> ecu_crc = read_crc(block);
        if (!ecu_crc.has_value())
        {
            return std::unexpected(ecu_crc.error());
        }
        const std::uint32_t image_crc = checksum::Crc32(addressed_image.subspan(block.start, block.length));
        modified[index] = *ecu_crc != image_crc;
        modified_count += modified[index] ? 1U : 0U;
    }
    return modified_count;
}

// The BEEF K-Line write over `blocks` of the addressed `image`: compares every
// block's CRC, enables programming voltage (also when nothing differs) and,
// when a block differs, initialises the flash write, reflashes each changed
// block after a 200 ms programming-voltage query, then compares again and
// reports. A final mismatch logs an error and still returns success.
template <typename ReadCrc, typename InitFlashWrite, typename FlashBlock>
Status DensoBeefKlineWriteChangedBlocks(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                                        IEventSink& events, std::span<const MemoryRegion> blocks, bytes::ByteView image,
                                        bool test_write, ReadCrc&& read_crc, InitFlashWrite&& init_flash_write,
                                        FlashBlock&& flash_block)
{
    std::vector<bool> modified(blocks.size(), false);
    const auto compare = [&] { return DensoBeefKlineCompareBlocks(blocks, image, modified, cancellation, read_crc); };
    events.Log(LogLevel::kInfo, "Comparing ECU flash memory pages to image file");
    Result<unsigned> modified_count = compare();
    if (!modified_count.has_value())
    {
        return std::unexpected(modified_count.error());
    }
    if (Status line = DensoBeefKlineEnableProgrammingVoltage(transport, cancellation); !line.has_value())
    {
        return line;
    }
    if (*modified_count == 0)
    {
        events.Log(LogLevel::kInfo, "No difference between ROM and ECU data, no flashing needed");
        return {};
    }
    if (Status initialized = init_flash_write(); !initialized.has_value())
    {
        return initialized;
    }
    for (std::size_t block_no = 0; block_no < blocks.size(); ++block_no)
    {
        if (!modified[block_no])
        {
            continue;
        }
        if (Result<bytes::Bytes> voltage =
                DensoBeefKlineExchangeAck(transport, nullptr, cancellation, DensoBeefKlineFrame(kDensoKlineOpProgVolt),
                                          std::chrono::milliseconds{0}, std::chrono::milliseconds{200},
                                          kDensoKlineOpProgVolt, "prog-volt query", 7);
            !voltage.has_value())
        {
            return std::unexpected(voltage.error());
        }
        if (Status flashed = flash_block(blocks[block_no]); !flashed.has_value())
        {
            return flashed;
        }
        events.Log(LogLevel::kInfo, "Block reflash complete");
    }
    events.Log(LogLevel::kInfo, "Comparing ECU flash memory pages to image file after reflash");
    return compare().transform(
        [&](unsigned remaining)
        {
            if (test_write)
            {
                events.Log(LogLevel::kInfo, "Test write PASS, it is safe to perform the actual write");
            }
            else if (remaining != 0)
            {
                events.Log(LogLevel::kError,
                           "Flash verification differs; do not power off, the kernel is still running");
            }
        });
}
} // namespace fastecu::flash
