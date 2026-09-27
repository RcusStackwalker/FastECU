#include "src/backend/diagnostics/ssm_identify.h"

#include <cstddef>
#include <format>
#include <string>

namespace fastecu::diagnostics
{
namespace
{

constexpr bytes::Byte kSsmHeader = 0x80;
constexpr bytes::Byte kSsmTester = 0xF0;
constexpr std::size_t kEcuIdOffset = 8;
constexpr std::size_t kEcuIdLength = 5;

bytes::Byte target_id(SsmTarget target)
{
    return target == SsmTarget::Ecu ? 0x10 : 0x18;
}

bytes::Byte ssm_checksum(bytes::ByteView data)
{
    unsigned sum = 0;
    for (const bytes::Byte byte : data)
    {
        sum += byte;
    }
    return static_cast<bytes::Byte>(sum & 0xFFU);
}

std::string upper_hex(bytes::ByteView data)
{
    std::string out;
    for (const bytes::Byte byte : data)
    {
        out += std::format("{:02X}", byte);
    }
    return out;
}

} // namespace

bytes::Bytes ssm_frame(bytes::ByteView payload, SsmTarget target)
{
    bytes::Bytes frame{kSsmHeader, target_id(target), kSsmTester, static_cast<bytes::Byte>(payload.size())};
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.push_back(ssm_checksum(frame));
    return frame;
}

std::optional<std::string> parse_ssm_ecu_id(bytes::ByteView init_response)
{
    if (init_response.size() < kEcuIdOffset + kEcuIdLength)
    {
        return std::nullopt;
    }
    return upper_hex(init_response.subspan(kEcuIdOffset, kEcuIdLength));
}

Result<SsmIdentity> identify_ssm_ecu(IDiagnosticLink& /*link*/, IClock& /*clock*/,
                                     const ICancellationToken& /*cancellation*/, const SsmIdentifyRequest& /*request*/)
{
    return fail(ErrorKind::Unsupported, "SSM identification is not implemented yet");
}

} // namespace fastecu::diagnostics
