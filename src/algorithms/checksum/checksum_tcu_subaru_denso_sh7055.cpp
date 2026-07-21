#include "checksum_tcu_subaru_denso_sh7055.h"
#include "src/algorithms/protocol/bytes.h"

#include <algorithm>

namespace
{

// Mirrors QByteArray::replace(pos, len, payload) for the same-size,
// in-bounds overwrite this file performs (len == payload.size() at the
// call site).
void overwriteAt(bytes::Bytes& data, std::size_t pos, bytes::ByteView payload)
{
    if (pos >= data.size())
    {
        return;
    }
    const std::size_t count = std::min(payload.size(), data.size() - pos);
    std::copy_n(payload.begin(), count, data.begin() + static_cast<std::ptrdiff_t>(pos));
}

// (area_start, word_count) pairs, transcribed 1:1 from the original
// QStringList of hex literals ("0x1000", "0x0800", ...) parsed at runtime
// via QString::toUInt(&ok, 16). Values are unchanged; only the
// string-parsing indirection is removed.
struct ChecksumArea
{
    uint32_t start;
    uint32_t wordCount;
};

constexpr ChecksumArea kChecksumAreas[] = {
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
};

} // namespace

ChecksumTcuSubaruDensoSH7055::ChecksumTcuSubaruDensoSH7055()
{
}

ChecksumTcuSubaruDensoSH7055::~ChecksumTcuSubaruDensoSH7055()
{
}

ChecksumResult ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(bytes::ByteView romView)
{
    /*******************
     *
     * Checksum is calulated by adding 16bit values from each area. As added bytes are 16bit,
     * every area size (16bit byte count) needs to multiply by 2
     *
     ******************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint16_t checksum = 0;

    for (const ChecksumArea& area : kChecksumAreas)
    {
        const uint32_t area_start = area.start;
        const uint32_t area_end = area_start + (2 * area.wordCount);

        for (uint32_t j = area_start; j < area_end; j += 2)
        {
            checksum += bytes::readU16Be(romData, j);
        }
    }

    ChecksumResult result;
    if (checksum != 0x5aa5)
    {
        bytes::Bytes balance_value_array;
        uint32_t balance_value_array_start = 0x7fff4;
        uint16_t balance_value = bytes::readU16Be(romData, 0x7fff4);

        balance_value += 0x5aa5 - checksum;

        bytes::appendU16Be(balance_value_array, balance_value);
        overwriteAt(romData, balance_value_array_start, balance_value_array);

        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Denso SH7055 TCU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    result.romData = romData;
    return result;
}
