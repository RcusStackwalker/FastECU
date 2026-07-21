#include "checksum_ecu_subaru_denso_sh705x_diesel.h"
#include "src/algorithms/protocol/qt_bytes.h"

ChecksumEcuSubaruDensoSH705xDiesel::ChecksumEcuSubaruDensoSH705xDiesel()
{
}

ChecksumEcuSubaruDensoSH705xDiesel::~ChecksumEcuSubaruDensoSH705xDiesel()
{
}

ChecksumResult ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(QByteArray romData, uint32_t checksum_area_start, uint32_t checksum_area_length)
{
    QByteArray checksum_array;

    uint32_t checksum_area_end = checksum_area_start + checksum_area_length;
    uint32_t checksum_dword_addr_lo = 0;
    uint32_t checksum_dword_addr_hi = 0;
    uint32_t checksum = 0;
    uint32_t checksum_temp = 0;
    uint32_t checksum_diff = 0;
    uint32_t checksum_check = 0;
    uint8_t checksum_block = 0;

    bool checksum_ok = true;
    bool sh72543_checksum_ok = true;

    for (uint32_t i = checksum_area_start; i < checksum_area_end; i += 12)
    {
        checksum = 0;
        checksum_temp = 0;
        checksum_dword_addr_lo = 0;
        checksum_dword_addr_hi = 0;
        checksum_diff = 0;

        checksum_dword_addr_lo = bytes::readU32Be(bytes::view(romData), i);
        checksum_dword_addr_hi = bytes::readU32Be(bytes::view(romData), i + 4);
        checksum_diff = bytes::readU32Be(bytes::view(romData), i + 8);
        if (i == checksum_area_start && checksum_dword_addr_lo == 0 && checksum_dword_addr_hi == 0 && checksum_diff == 0x5aa5a55a)
        {
            qDebug() << "ROM has all checksums disabled";
            ChecksumResult result;
            result.status = ChecksumResult::Status::Disabled;
            // Preserves the legacy calculate_checksum() contract: this early-return
            // path returned QByteArray() (via `return 0;`), so callers that assign
            // the result straight into FullRomData see it wiped, not the original
            // ROM bytes. Not changed here — only the dialog is being removed.
            result.romData = QByteArray();
            result.message = QObject::tr("ROM has all checksums disabled");
            return result;
        }

        if (checksum_dword_addr_lo != 0 && checksum_dword_addr_hi != 0 && checksum_diff != 0x5aa5a55a)
        {
            for (uint32_t j = checksum_dword_addr_lo; j < checksum_dword_addr_hi; j += 4)
            {
                checksum_temp = bytes::readU32Be(bytes::view(romData), j);
                if (checksum_area_start == 0x0FFB80 && j == 0x0FFAFC)
                {
                    checksum += 0xFFFFFFFF;
                }
                else if (checksum_area_start == 0x17FB80 && j == 0x17FAFC)
                {
                    checksum += 0xFFFFFFFF;
                }
                else
                {
                    checksum += checksum_temp;
                }
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

        bytes::appendU32Be(checksum_array, checksum_dword_addr_lo);
        bytes::appendU32Be(checksum_array, checksum_dword_addr_hi);
        bytes::appendU32Be(checksum_array, checksum_check);

        checksum_block++;
    }

    if (!checksum_ok)
    {
        romData.replace(checksum_area_start, checksum_area_length, checksum_array);
        qDebug() << "Checksums corrected";
    }

    // Check SH72543 EURO6 Diesel additional checksums
    if (checksum_area_start == 0x1FF800)
    {
        checksum_array.clear();
        checksum_area_start = 0x001FF8E8;
        checksum_area_length = 2 * 12;
        checksum_area_end = checksum_area_start + checksum_area_length;

        for (uint32_t i = checksum_area_start; i < checksum_area_end; i += 12)
        {
            checksum = 0;
            checksum_temp = 0;
            checksum_dword_addr_lo = 0;
            checksum_dword_addr_hi = 0;
            checksum_diff = 0;

            checksum_dword_addr_lo = bytes::readU32Be(bytes::view(romData), i);
            checksum_dword_addr_hi = bytes::readU32Be(bytes::view(romData), i + 4);
            checksum_diff = bytes::readU32Be(bytes::view(romData), i + 8);

            if (checksum_dword_addr_lo != 0 && checksum_dword_addr_hi != 0 && checksum_diff != 0x5aa5a55a)
            {
                for (uint32_t j = checksum_dword_addr_lo; j < checksum_dword_addr_hi; j += 4)
                {
                    checksum_temp = bytes::readU32Be(bytes::view(romData), j);
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
                sh72543_checksum_ok = false;
            }

            bytes::appendU32Be(checksum_array, checksum_dword_addr_lo);
            bytes::appendU32Be(checksum_array, checksum_dword_addr_hi);
            bytes::appendU32Be(checksum_array, checksum_check);

            checksum_block++;
        }

        if (!sh72543_checksum_ok)
        {
            romData.replace(checksum_area_start, checksum_area_length, checksum_array);
            qDebug() << "SH72543 EURO6 additional checksums corrected";
        }
    }
    ChecksumResult result;
    result.romData = romData;
    if (!checksum_ok || !sh72543_checksum_ok)
    {
        result.status = ChecksumResult::Status::Corrected;
        result.message = QObject::tr("Subaru Denso SH705x Checksum");
    }
    else
    {
        result.status = ChecksumResult::Status::Unchanged;
    }
    return result;
}
