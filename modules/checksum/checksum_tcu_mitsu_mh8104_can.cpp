#include "checksum_tcu_mitsu_mh8104_can.h"
#include "protocol/qt_bytes.h"

ChecksumTcuMitsuMH8104Can::ChecksumTcuMitsuMH8104Can()
{
}

ChecksumTcuMitsuMH8104Can::~ChecksumTcuMitsuMH8104Can()
{
}

QByteArray ChecksumTcuMitsuMH8104Can::calculate_checksum(QByteArray romData)
{
    /****************************
     *
     *  FUN_00001578: Check that 0x8000 = 0x5aa5 & 0x7fffe = 0xa55a
     *  FUN_0000288c; Check that
     *
     *
     *
     * *************************/

    uint32_t checksum_balance_value = 0;
    uint32_t checksum_balance_value_address = 0x81fc;
    uint32_t checksum_target = 0x5aa45aab;

    QByteArray msg;
    uint32_t checksum = 0;

    bool checksum_ok = true;

    for (int i = 0x8000; i < 0x80000; i += 4)
    {
        checksum += bytes::readU32Be(bytes::view(romData), static_cast<std::size_t>(i));
    }
    checksum -= 0xffff;
    for (int j = 0; j < 5; j++)
    {
        checksum -= 0xffffffff;
    }
    msg.clear();
    msg.append(QString("Checksum calculated: 0x%1").arg(checksum, 8, 16, QLatin1Char('0')).toUtf8());
    qDebug() << msg;

    if (checksum != checksum_target)
    {
        qDebug() << "Checksum value mismatch!";
        checksum_ok = false;

        QByteArray balance_value_array;

        checksum_balance_value = bytes::readU32Be(bytes::view(romData), checksum_balance_value_address);

        msg.clear();
        msg.append(QString("Balance value before: 0x%1").arg(checksum_balance_value, 8, 16, QLatin1Char('0')).toUtf8());
        qDebug() << msg;

        if (checksum > checksum_target)
        {
            checksum_balance_value += checksum_target - checksum;
        }
        else
        {
            checksum_balance_value += checksum_target - checksum;
        }
        msg.clear();
        msg.append(QString("Balance value after: 0x%1").arg(checksum_balance_value, 8, 16, QLatin1Char('0')).toUtf8());
        qDebug() << msg;

        bytes::appendU32Be(balance_value_array, checksum_balance_value);
        romData.replace(checksum_balance_value_address, balance_value_array.length(), balance_value_array);

        qDebug() << "Subaru Mitsu MH8104 CAN CVT TCU checksum corrected";
    }
    else
    {
        qDebug() << "Subaru Mitsu MH8104 CAN CVT TCU checksum OK";
    }
    if (!checksum_ok)
    {
        QMessageBox::information(nullptr, QObject::tr("Subaru Hitachi M32R K-Line/CAN ECU Checksum"), "Checksums corrected");
    }

    return romData;
}
