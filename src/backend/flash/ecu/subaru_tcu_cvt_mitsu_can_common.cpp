#include "src/backend/flash/ecu/subaru_tcu_cvt_mitsu_can_common.h"

namespace fastecu::flash
{
namespace
{
constexpr std::array<std::uint16_t, 16> kSeedKeyTable{
    0x9E99, 0x685C, 0x874D, 0xF11E, 0x27D4, 0xA967, 0xB63B, 0x7A37,
    0xE23B, 0xA8D0, 0x9B82, 0xAC43, 0xE874, 0x7FC5, 0x7141, 0x8B44};
constexpr std::array<std::uint16_t, 4> kEncryptTable{0x7bf2, 0xa8b4, 0x4492, 0x6587};
constexpr std::array<std::uint16_t, 4> kDecryptTable{0x6587, 0x4492, 0xa8b4, 0x7bf2};
} // namespace

const std::array<std::uint16_t, 16>& tcuCvtMitsuSeedKeyTable()
{
    return kSeedKeyTable;
}

const std::array<std::uint16_t, 4>& tcuCvtMitsuEncryptTable()
{
    return kEncryptTable;
}

const std::array<std::uint16_t, 4>& tcuCvtMitsuDecryptTable()
{
    return kDecryptTable;
}

} // namespace fastecu::flash
