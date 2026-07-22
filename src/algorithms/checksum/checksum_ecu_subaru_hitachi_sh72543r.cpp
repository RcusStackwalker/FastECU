#include "checksum_ecu_subaru_hitachi_sh72543r.h"
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

} // namespace

ChecksumEcuSubaruHitachiSh72543r::ChecksumEcuSubaruHitachiSh72543r()
{
}

ChecksumEcuSubaruHitachiSh72543r::~ChecksumEcuSubaruHitachiSh72543r()
{
}

ChecksumResult ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::ByteView romView)
{
    /*******************
     *
     * Checksum is calculated between 0x6000 - 0x1fffff, 16bit summation, balance value is word at 0x1ffffe
     * PTR_DAT_000b446c
     *
     ******************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint16_t chksum = 0;

    for (int i = 0x6000; i < 0x200000; i += 2)
    {
        chksum += bytes::readU16Be(romData, static_cast<std::size_t>(i));
    }

    ChecksumResult result;
    if (chksum != 0x5aa5)
    {
        bytes::Bytes balance_value_array;
        uint32_t balance_value_array_start = 0x1ffffe;
        uint16_t balance_value = bytes::readU16Be(romData, balance_value_array_start);

        balance_value += 0x5aa5 - chksum;

        bytes::appendU16Be(balance_value_array, balance_value);
        overwriteAt(romData, balance_value_array_start, balance_value_array);

        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi SH72543r ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    result.romData = romData;
    return result;
}
