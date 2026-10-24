#include "src/algorithms/protocol/uds/uds_pdu.h"

#include "src/algorithms/protocol/bytes_compose.h"

namespace uds
{

std::array<bytes::Byte, 1> BuildRequest(bytes::Byte sid)
{
    return bytes::ComposeBe(sid);
}

std::array<bytes::Byte, 2> BuildRequest(bytes::Byte sid, bytes::Byte subfunction)
{
    return bytes::ComposeBe(sid, subfunction);
}

bytes::Bytes BuildRequest(bytes::Byte sid, bytes::ByteView data)
{
    return bytes::ComposeBe(sid, data);
}

bytes::Bytes BuildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data)
{
    return bytes::ComposeBe(sid, subfunction, data);
}

} // namespace uds
