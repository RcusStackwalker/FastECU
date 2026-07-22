#include "checksum_tcu_mitsu_mh8104_can.h"
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

ChecksumTcuMitsuMH8104Can::ChecksumTcuMitsuMH8104Can()
{
}

ChecksumTcuMitsuMH8104Can::~ChecksumTcuMitsuMH8104Can()
{
}

ChecksumResult ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::ByteView romView)
{
    /****************************
     *
     *  FUN_00001578: Check that 0x8000 = 0x5aa5 & 0x7fffe = 0xa55a
     *  FUN_0000288c; Check that
     *
     *
     *
     * *************************/
    bytes::Bytes romData(romView.begin(), romView.end());

    uint32_t checksum_balance_value = 0;
    uint32_t checksum_balance_value_address = 0x81fc;
    uint32_t checksum_target = 0x5aa45aab;

    uint32_t checksum = 0;

    bool checksum_ok = true;

    for (int i = 0x8000; i < 0x80000; i += 4)
    {
        checksum += bytes::readU32Be(romData, static_cast<std::size_t>(i));
    }
    checksum -= 0xffff;
    for (int j = 0; j < 5; j++)
    {
        checksum -= 0xffffffff;
    }

    if (checksum != checksum_target)
    {
        checksum_ok = false;

        bytes::Bytes balance_value_array;

        checksum_balance_value = bytes::readU32Be(romData, checksum_balance_value_address);

        if (checksum > checksum_target)
        {
            checksum_balance_value += checksum_target - checksum;
        }
        else
        {
            checksum_balance_value += checksum_target - checksum;
        }

        bytes::appendU32Be(balance_value_array, checksum_balance_value);
        overwriteAt(romData, checksum_balance_value_address, balance_value_array);
    }
    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Subaru Hitachi M32R K-Line/CAN ECU Checksum";
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
