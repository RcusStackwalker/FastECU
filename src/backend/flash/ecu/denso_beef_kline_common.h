#pragma once

#include <chrono>
#include <span>
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

// Callers provide valid absolute-addressed image slices and one flag per block.
template <class ReadCrc>
Result<unsigned> DensoBeefKlineCompareBlocks(std::span<const MemoryRegion> blocks, bytes::ByteView addressed_image,
                                             std::vector<bool>& modified, const ICancellationToken& cancellation,
                                             ReadCrc&& read_crc)
{
    unsigned modified_count = 0;
    for (std::size_t index = 0; index < blocks.size(); ++index)
    {
        if (cancellation.Cancelled())
            return Fail(ErrorKind::kCancelled, "cancelled during ROM compare");
        const MemoryRegion& block = blocks[index];
        const Result<std::uint32_t> ecu_crc = read_crc(block);
        if (!ecu_crc.has_value())
            return std::unexpected(ecu_crc.error());
        const std::uint32_t image_crc = checksum::Crc32(addressed_image.subspan(block.start, block.length));
        modified[index] = *ecu_crc != image_crc;
        modified_count += modified[index] ? 1U : 0U;
    }
    return modified_count;
}
} // namespace fastecu::flash
