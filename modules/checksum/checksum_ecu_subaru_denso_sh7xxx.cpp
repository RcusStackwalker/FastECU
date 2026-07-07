#include "checksum_ecu_subaru_denso_sh7xxx.h"

ChecksumEcuSubaruDensoSH7xxx::ChecksumEcuSubaruDensoSH7xxx()
{

}

ChecksumEcuSubaruDensoSH7xxx::~ChecksumEcuSubaruDensoSH7xxx()
{

}

QByteArray ChecksumEcuSubaruDensoSH7xxx::calculate_checksum(QByteArray romData, uint32_t checksum_area_start, uint32_t checksum_area_length, int32_t offset)
{
    return calculate_checksum_result(romData, checksum_area_start, checksum_area_length, offset).romData;
}

ChecksumResult ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(QByteArray romData, uint32_t checksum_area_start, uint32_t checksum_area_length, int32_t offset)
{
    QByteArray checksum_array;
    ChecksumResult result;
    result.romData = romData;

    if (checksum_area_length % 12 != 0) {
        result.status = ChecksumResult::Status::ParseError;
        result.message = "Checksum area length must be a multiple of 12 bytes";
        return result;
    }

    if (checksum_area_start > static_cast<uint32_t>(romData.size()) ||
        checksum_area_length > static_cast<uint32_t>(romData.size()) - checksum_area_start) {
        result.status = ChecksumResult::Status::InvalidSize;
        result.message = "ROM is too small for the configured checksum area";
        return result;
    }

    uint32_t checksum_area_end = checksum_area_start + checksum_area_length;
    uint32_t checksum_dword_addr_lo = 0;
    uint32_t checksum_dword_addr_hi = 0;
    uint32_t checksum = 0;
    uint32_t checksum_temp = 0;
    uint32_t checksum_diff = 0;
    uint32_t checksum_check = 0;
    uint8_t checksum_block = 0;

    bool checksum_ok = true;


    for (uint32_t i = checksum_area_start; i < checksum_area_end; i+=12)
    {
        checksum = 0;
        checksum_temp = 0;
        checksum_dword_addr_lo = 0;
        checksum_dword_addr_hi = 0;
        checksum_diff = 0;

        for (int j = 0; j < 4; j++)
        {
            checksum_dword_addr_lo = (checksum_dword_addr_lo << 8) + (uint8_t)romData.at(i + j);
            checksum_dword_addr_hi = (checksum_dword_addr_hi << 8) + (uint8_t)romData.at(i + 4 + j);
            checksum_diff = (checksum_diff << 8) + (uint8_t)romData.at(i + 8 + j);
        }
        if (checksum_dword_addr_lo == 0 && checksum_dword_addr_hi == 0)
        {
            offset = 0;
        }
        uint32_t checksum_dword_addr_lo_with_offset = checksum_dword_addr_lo + offset;
        uint32_t checksum_dword_addr_hi_with_offset = checksum_dword_addr_hi + offset;
        if (i == checksum_area_start && checksum_dword_addr_lo_with_offset == 0 && checksum_dword_addr_hi_with_offset == 0 && checksum_diff == 0x5aa5a55a)
        {
            qDebug() << "ROM has all checksums disabled";
            result.status = ChecksumResult::Status::Disabled;
            result.message = "ROM has all checksums disabled";
            return result;
        }

        if (checksum_dword_addr_lo_with_offset == 0 && checksum_dword_addr_hi_with_offset == 0 && checksum_diff == 0x5aa5a55a)
        {
            //QMessageBox::information(nullptr, QObject::tr("32-bit checksum"), "Checksums disabled");
        }
        if (checksum_dword_addr_lo_with_offset != 0 && checksum_dword_addr_hi_with_offset != 0 && checksum_diff != 0x5aa5a55a)
        {
            for (uint32_t j = checksum_dword_addr_lo_with_offset; j < checksum_dword_addr_hi_with_offset; j+=4)
            {
                if (j > static_cast<uint32_t>(romData.size()) || 4 > static_cast<uint32_t>(romData.size()) - j) {
                    result.status = ChecksumResult::Status::InvalidSize;
                    result.message = "ROM is too small for a checksum block range";
                    return result;
                }
                for (int k = 0; k < 4; k++)
                {
                    checksum_temp = (checksum_temp << 8) + (uint8_t)(romData.at(j + k));
                }
                checksum += checksum_temp;
            }
        }
        checksum_check = 0x5aa5a55a - checksum;

        if (checksum_diff == checksum_check)
        {
            qDebug() << "Checksum block " + QString::number(checksum_block) + " OK";
        }
        else
        {
            qDebug() << "Checksum block " + QString::number(checksum_block) + " NOK";
            checksum_ok = false;
        }

        checksum_array.append(checksum_dword_addr_lo >> 24);
        checksum_array.append(checksum_dword_addr_lo >> 16);
        checksum_array.append(checksum_dword_addr_lo >> 8);
        checksum_array.append(checksum_dword_addr_lo);
        checksum_array.append(checksum_dword_addr_hi >> 24);
        checksum_array.append(checksum_dword_addr_hi >> 16);
        checksum_array.append(checksum_dword_addr_hi >> 8);
        checksum_array.append(checksum_dword_addr_hi);
        checksum_array.append(checksum_check >> 24);
        checksum_array.append(checksum_check >> 16);
        checksum_array.append(checksum_check >> 8);
        checksum_array.append(checksum_check);

        checksum_block++;
    }

    if (!checksum_ok)
    {
        result.romData.replace(checksum_area_start, checksum_area_length, checksum_array);
        result.status = ChecksumResult::Status::Corrected;
        result.message = "Checksums corrected";
        qDebug() << "Checksums corrected";
    }
    else {
        result.status = ChecksumResult::Status::Unchanged;
        result.message = "Checksums OK";
    }

    return result;
}
