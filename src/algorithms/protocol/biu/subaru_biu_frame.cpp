#include "src/algorithms/protocol/biu/subaru_biu_frame.h"

namespace biu_subaru
{

namespace
{

constexpr bytes::Byte kFormatBase = 0x80;
constexpr bytes::Byte kTarget = 0x40;
constexpr bytes::Byte kSource = 0xF0;
constexpr bytes::Byte kTesterPresent = 0x3E;

} // namespace

bytes::Byte checksum(bytes::ByteView data, bool excludeLast)
{
    if (excludeLast && !data.empty())
    {
        data = data.first(data.size() - 1);
    }

    bytes::Byte sum = 0;
    for (const bytes::Byte value : data)
    {
        sum = static_cast<bytes::Byte>(sum + value);
    }
    return sum;
}

bytes::Bytes buildRequest(bytes::ByteView payload)
{
    bytes::Bytes frame{static_cast<bytes::Byte>(kFormatBase | payload.size()), kTarget, kSource};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(checksum(frame, false));
    return frame;
}

bytes::Bytes keepAliveRequest()
{
    const bytes::Bytes payload{kTesterPresent};
    return buildRequest(payload);
}

} // namespace biu_subaru
