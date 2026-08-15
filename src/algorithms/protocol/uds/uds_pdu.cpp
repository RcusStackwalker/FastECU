#include "src/algorithms/protocol/uds/uds_pdu.h"

#include "src/algorithms/protocol/bytes_compose.h"

namespace uds
{

bytes::Bytes buildRequest(bytes::Byte sid)
{
    return bytes::composeBe(sid);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction)
{
    return bytes::composeBe(sid, subfunction);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::ByteView data)
{
    return bytes::composeBe(sid, data);
}

bytes::Bytes buildRequest(bytes::Byte sid, bytes::Byte subfunction, bytes::ByteView data)
{
    return bytes::composeBe(sid, subfunction, data);
}

} // namespace uds
