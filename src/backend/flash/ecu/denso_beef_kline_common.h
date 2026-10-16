#pragma once

#include <chrono>
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
} // namespace fastecu::flash
