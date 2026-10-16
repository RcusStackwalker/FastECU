#pragma once

#include "src/backend/protocol/uds/iuds_channel.h"

#include <cstdint>

namespace fastecu::flash
{
// CVT chunk acknowledgement policy: silence and every framed reply succeed.
// The caller owns the wire address window, encryption and cancellation checks.
Status ExchangeCvtB6Chunk(uds::IUdsChannel& channel, const ICancellationToken& cancellation, std::uint32_t address,
                          bytes::ByteView encrypted_chunk);
} // namespace fastecu::flash
