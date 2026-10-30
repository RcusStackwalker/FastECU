#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"

#include <array>
#include <cstdint>

#include "src/algorithms/protocol/ssm/ssm_protocol_core.h"

namespace fastecu::flash
{
namespace
{
// Tables confirmed byte-identical between both legacy families; citations
// establish provenance only, not hardware qualification. Seed key: generate_seed_key
// in both mh8111 and mh8104.
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
                                                      0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
// Write encryption: encrypt_payload in both.
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
// Read decryption: decrypt_payload in both.
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x6587, 0x4492, 0xa8b4, 0x7bf2};
} // namespace

bytes::Bytes TcuCvtMitsuSeedKey(bytes::ByteView seed)
{
    return ssm_protocol::CalculateSeedKey(seed, kSeedKeyTable, ssm_protocol::kStockPairTable);
}

bytes::Bytes TcuCvtMitsuEncryptRom(bytes::ByteView image)
{
    return ssm_protocol::CalculatePayload(image, kEncryptTable, ssm_protocol::kStockPairTable);
}

bytes::Bytes TcuCvtMitsuDecryptPage(bytes::ByteView page)
{
    return ssm_protocol::CalculatePayload(page, kDecryptTable, ssm_protocol::kStockPairTable);
}

} // namespace fastecu::flash
