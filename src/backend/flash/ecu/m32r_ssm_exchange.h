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
Result<std::optional<bytes::Bytes>> ExchangeM32rSsmOptional(IKlineFlashTransport& transport,
                                                            const ICancellationToken& cancellation,
                                                            bytes::ByteView payload, bytes::Byte tester_id,
                                                            bytes::Byte target_id, std::chrono::milliseconds timeout);

} // namespace fastecu::flash
