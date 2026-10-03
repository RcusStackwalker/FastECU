#include "src/algorithms/protocol/biu/subaru_biu_frame.h"

#include "src/algorithms/protocol/bytes_compose.h"

namespace biu_subaru
{

namespace
{

constexpr bytes::Byte kFormatBase = 0x80;
constexpr bytes::Byte kTarget = 0x40;
constexpr bytes::Byte kSource = 0xF0;
constexpr bytes::Byte kTesterPresent = 0x3E;

} // namespace

bytes::Bytes buildRequest(bytes::ByteView payload)
{
    return bytes::composeBeWithChecksum(bytes::sum8, static_cast<bytes::Byte>(kFormatBase | payload.size()), kTarget,
                                        kSource, payload);
}

bytes::Bytes keepAliveRequest()
{
    const bytes::Bytes payload{kTesterPresent};
    return buildRequest(payload);
}

} // namespace biu_subaru
