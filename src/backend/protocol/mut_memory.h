#pragma once
#include "src/algorithms/protocol/bytes.h"
#include "src/backend/ports/cancellation.h"
#include "src/backend/ports/result.h"
#include "src/backend/protocol/ikline_transport.h"

#include <cstddef>
#include <cstdint>

namespace mutdma
{

// Standalone MUT/DMA memory access over an ECU already in MUT/DMA mode at
// 125000 baud. Moved from MainWindow (step 6g), where nothing called it.

// Requires a start in 0x4000-0xBFFF and the entire write interval within that
// writable RAM window; otherwise returns InvalidConfig before any I/O. Empty
// writes succeed only at valid starts. Do not relax the window.
fastecu::Status WriteMemory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                            const fastecu::ICancellationToken& cancellation);

// Success returns exactly len contiguous bytes in chunks of up to 40 bytes.
// Rejects ranges beyond the 16-bit address space with InvalidConfig before I/O;
// zero length succeeds without I/O. Any setup or poll error aborts immediately,
// a missing frame returns Timeout, and an unexpected chunk length is BadResponse.
fastecu::Result<bytes::Bytes> ReadMemory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                         const fastecu::ICancellationToken& cancellation);

} // namespace mutdma
