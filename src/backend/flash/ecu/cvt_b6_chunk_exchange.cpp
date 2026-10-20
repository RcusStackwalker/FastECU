#include "src/backend/flash/ecu/cvt_b6_chunk_exchange.h"

#include "src/algorithms/protocol/bytes_compose.h"
#include "src/algorithms/protocol/uds/uds_service_ids.h"

#include <chrono>

namespace fastecu::flash
{
Status ExchangeCvtB6Chunk(uds::IUdsChannel& channel, const ICancellationToken& cancellation, std::uint32_t address,
                          bytes::ByteView encrypted_chunk)
{
    if (const Status sent = channel.Send(
            bytes::ComposeBe(uds::kSidWriteMemoryChunk, bytes::U24{address}, encrypted_chunk), cancellation);
        !sent.has_value())
    {
        return sent;
    }
    if (const auto reply = channel.Receive(std::chrono::milliseconds{2000}, cancellation); !reply.has_value())
    {
        return std::unexpected(reply.error());
    }
    return {};
}
} // namespace fastecu::flash
