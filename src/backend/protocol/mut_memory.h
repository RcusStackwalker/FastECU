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

// Refuses addresses outside the writable RAM window 0x4000-0xBFFF with
// InvalidConfig before any I/O. Do not relax the window.
fastecu::Status write_memory(IKlineTransport& transport, std::uint16_t addr, bytes::ByteView data,
                             const fastecu::ICancellationToken& cancellation);

// Reads in chunks of up to 40 bytes. Returns what was read before the first
// failed chunk; fails only if the first chunk fails. A chunk whose poll yields
// no frame contributes nothing and the read continues.
fastecu::Result<bytes::Bytes> read_memory(IKlineTransport& transport, std::uint16_t addr, std::size_t len,
                                          const fastecu::ICancellationToken& cancellation);

} // namespace mutdma
