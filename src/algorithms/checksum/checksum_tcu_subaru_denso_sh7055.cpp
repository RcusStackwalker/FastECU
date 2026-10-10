#include "checksum_tcu_subaru_denso_sh7055.h"

#include <utility>

#include "checksum_primitives.h"
#include "src/algorithms/protocol/bytes.h"
#include <array>

namespace
{

// (area_start, word_count) pairs, transcribed 1:1 from the original
// QStringList of hex literals ("0x1000", "0x0800", ...) parsed at runtime
// via QString::toUInt(&ok, 16). Values are unchanged; only the
// string-parsing indirection is removed.
struct ChecksumArea
{
    uint32_t start;
    uint32_t word_count;
};

constexpr std::array<ChecksumArea, 12> kChecksumAreas{{
    {0x1000, 0x0800},
    {0x3000, 0x003a},
    {0x3080, 0x4c00},
    {0xc880, 0x83f6},
    {0x1d06c, 0x83f6},
    {0x2d858, 0x83f8},
    {0x3e048, 0x0fdc},
    {0x40000, 0x741c},
    {0x4e838, 0x83f6},
    {0x5f024, 0x83f8},
    {0x6f814, 0x8174},
    {0x7fb80, 0x0240},
}};

} // namespace

ChecksumResult ChecksumTcuSubaruDensoSH7055::CalculateChecksumResult(const fastecu::memory::MemoryView& rom)
{
    if (auto rejected = fastecu::checksum::internal::RequireStartAtZero(rom); rejected.has_value())
    {
        return *std::move(rejected);
    }
    const bytes::ByteView rom_view = rom.Data();
    // Fixed 512 KiB SH7055 layout; the last checksum area ends at 0x80000.
    if (rom_view.size() != 0x80000)
    {
        return {.status = ChecksumResult::Status::kInvalidSize,
                .rom_data = bytes::Bytes(rom_view.begin(), rom_view.end()),
                .message = "ROM size does not match the checksum layout"};
    }
    /*******************
     *
     * Checksum is calulated by adding 16bit values from each area. As added bytes are 16bit,
     * every area size (16bit byte count) needs to multiply by 2
     *
     ******************/
    bytes::Bytes rom_data(rom_view.begin(), rom_view.end());

    uint16_t checksum = 0;

    for (const ChecksumArea& area : kChecksumAreas)
    {
        const uint32_t area_start = area.start;
        const uint32_t area_end = area_start + (2 * area.word_count);

        for (uint32_t j = area_start; j < area_end; j += 2)
        {
            checksum += bytes::ReadU16Be(rom_data, j);
        }
    }

    ChecksumResult result;
    if (checksum != 0x5aa5)
    {
        fastecu::checksum::internal::RebalanceU16Be(rom_data, 0x7fff4, checksum, 0x5aa5);

        result.status = ChecksumResult::Status::kCorrected;
        result.message = "Subaru Denso SH7055 TCU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::kUnchanged;
    }
    result.rom_data = rom_data;
    return result;
}
