#pragma once

#include <chrono>
#include <optional>

#include "src/backend/flash/flash_executor.h"

namespace fastecu::flash
{

// Shared by Subaru Hitachi M32R K-Line ECU, Subaru Mitsubishi M32R K-Line ECU,
// and Subaru TCU Hitachi M32R K-Line executors. Their ordinary optional SSM
// exchanges have equivalent framing, write-size checks, cancellation checkpoints
// and transport-error precedence. Replies are returned unchanged without frame
// validation; family parsing, pacing and timeout policy remain with callers.
// Composes SendM32rSsmFrame and ReceiveM32rSsmOnce with an additional
// after-write cancellation check before receiving.
Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(IKlineFlashTransport& transport,
                                                            const ICancellationToken& cancellation,
                                                            bytes::ByteView payload, bytes::Byte tester_id,
                                                            bytes::Byte target_id, std::chrono::milliseconds timeout);

// Separate operations shared by Unisia Jecs M32R K-Line and bootmode program
// executors. Send checks cancellation only before writing, frames with AddHeader
// (including its one-byte length wrap), propagates write errors and rejects a
// count unequal to the full frame size. There is no after-write checkpoint.
Status SendM32rSsmFrame(IKlineFlashTransport& transport, const ICancellationToken& cancellation,
                        bytes::ByteView payload, bytes::Byte tester_id, bytes::Byte target_id);

// Reads exactly once with the caller's timeout, even if already cancelled.
// Transport errors win over the after-read cancellation check. Optional replies
// (including present empty or malformed bytes) pass through without validation.
Result<std::optional<bytes::Bytes>> ReceiveM32rSsmOnce(IKlineFlashTransport& transport,
                                                       const ICancellationToken& cancellation,
                                                       std::chrono::milliseconds timeout);

// Compares payload bytes at offset four only; does not validate the envelope.
bool M32rSsmPayloadHasPrefix(bytes::ByteView response, bytes::ByteView prefix);

} // namespace fastecu::flash
