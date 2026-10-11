#pragma once
#include <array>
#include <cstdint>

#include "src/algorithms/protocol/bytes.h"
#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

// Crypto tables shared by the two Denso SH705x K-Line executors:
// DensoSh705xEepromKlineExecutor (step 5c) and SubaruDensoSh705xKlineExecutor.
// Both legacy classes spelled out these exact tables; nothing
// else about the two protocols is shared -- their kernel upload, reply
// checks and timeouts differ on the wire. See the original port design.
//
// The executor suites do NOT read these back: each carries its own
// transcribed literals, so a wrong change here fails those suites.
namespace fastecu::flash
{

inline constexpr std::array<std::uint16_t, 16> kDensoSh705xKlineSeedKeyTable{
    0x53DA, 0x33BC, 0x72EB, 0x437D, 0x7CA3, 0x3382, 0x834F, 0x3608,
    0xAFB8, 0x503D, 0xDBA3, 0x9D34, 0x3563, 0x6B70, 0x6E74, 0x88F0};

inline constexpr std::array<std::uint16_t, 4> kDensoSh705xKlineEncryptTable{0x7856, 0xCE22, 0xF513, 0x6E86};

inline bytes::Bytes DensoSh705xKlineStockSeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kDensoSh705xKlineSeedKeyTable, ssm_protocol::kIndexTransformationStock);
}

inline bytes::Bytes DensoSh705xKlineEcutekSeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kDensoSh705xKlineSeedKeyTable,
                                          ssm_protocol::kIndexTransformationEcutek);
}

inline bytes::Bytes DensoSh705xKlineEncryptPayload(bytes::ByteView buf, std::uint32_t len)
{
    return ssm_protocol::CalculatePayload(buf, len, kDensoSh705xKlineEncryptTable,
                                          ssm_protocol::kIndexTransformationStock);
}

} // namespace fastecu::flash
