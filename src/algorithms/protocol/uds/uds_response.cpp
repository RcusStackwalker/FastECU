#include "src/algorithms/protocol/uds/uds_response.h"

#include "src/algorithms/diagnostics/nrc_parser.h"

namespace uds
{

Response ParseResponse(bytes::ByteView pdu)
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
        return {ResponseKind::kNegative, pdu[1], pdu[2], pdu.subspan(3)};
    }
    if (pdu[0] < kPositiveResponseOffset)
    {
        return {};
    }
    return {ResponseKind::kPositive, RequestFromPositive(pdu[0]), 0, pdu.subspan(1)};
}

bytes::ByteView Payload(bytes::ByteView pdu)
{
    return pdu.empty() ? bytes::ByteView{} : pdu.subspan(1);
}

std::optional<bytes::Byte> Subfunction(bytes::ByteView pdu)
{
    if (pdu.size() < 2)
    {
        return std::nullopt;
    }
    return pdu[1];
}

std::string Describe(bytes::ByteView pdu)
{
    return NrcDescription(pdu);
}

} // namespace uds
