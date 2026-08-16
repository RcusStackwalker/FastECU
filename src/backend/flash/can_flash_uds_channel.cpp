#include "src/backend/flash/can_flash_uds_channel.h"

#include <format>
#include <utility>

#include "src/algorithms/protocol/bytes_compose.h"

namespace fastecu::flash
{

CanFlashUdsChannel::CanFlashUdsChannel(ICanFlashTransport& transport, std::uint32_t request_id,
                                       std::uint32_t response_id)
    : transport_(transport), request_id_(request_id), response_id_(response_id)
{
}

Status CanFlashUdsChannel::send(bytes::ByteView pdu, const ICancellationToken& cancellation)
{
    return transport_.write(bytes::composeBe(request_id_, pdu), cancellation);
}

Result<std::optional<bytes::Bytes>> CanFlashUdsChannel::receive(int timeout_ms, const ICancellationToken& cancellation)
{
    Result<std::optional<bytes::Bytes>> frame = transport_.read(timeout_ms, cancellation);
    if (!frame.has_value())
    {
        return std::unexpected(frame.error());
    }
    if (!frame->has_value())
    {
        return std::optional<bytes::Bytes>{};
    }

    const bytes::Bytes& raw = **frame;
    if (raw.size() < kEnvelopeSize)
    {
        return fail(ErrorKind::BadResponse,
                    std::format("CAN frame of {} bytes is shorter than its 4-byte id envelope", raw.size()));
    }

    const std::uint32_t id = bytes::readU32Be(raw);
    if (id != response_id_)
    {
        return fail(ErrorKind::BadResponse, std::format("expected CAN reply id 0x{:x}, got 0x{:x}", response_id_, id));
    }

    return std::optional<bytes::Bytes>(
        bytes::Bytes(raw.begin() + static_cast<std::ptrdiff_t>(kEnvelopeSize), raw.end()));
}

} // namespace fastecu::flash
