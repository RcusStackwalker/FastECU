#include "src/algorithms/protocol/uds/uds_response.h"

#include "src/algorithms/diagnostics/nrc_parser.h"

namespace uds
{

Response parseResponse(bytes::ByteView pdu)
{
    if (pdu.empty())
    {
        return {};
    }
    if (pdu[0] == kNegativeResponse)
    {
        if (pdu.size() < 3)
        {
            return {};
        }
        return {ResponseKind::Negative, pdu[1], pdu[2], pdu.subspan(3)};
    }
    if (pdu[0] < kPositiveResponseOffset)
    {
        return {};
    }
    return {ResponseKind::Positive, requestFromPositive(pdu[0]), 0, pdu.subspan(1)};
}

bytes::ByteView payload(bytes::ByteView pdu)
{
    return pdu.empty() ? bytes::ByteView{} : pdu.subspan(1);
}

std::optional<bytes::Byte> subfunction(bytes::ByteView pdu)
{
    if (pdu.size() < 2)
    {
        return std::nullopt;
    }
    return pdu[1];
}

std::string describe(bytes::ByteView pdu)
{
    return nrc_description(pdu);
}

} // namespace uds
