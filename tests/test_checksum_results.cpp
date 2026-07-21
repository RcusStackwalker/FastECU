#include <QtTest>
#include <QApplication>

#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh705x_diesel.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_denso_sh7xxx.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_m32r_kline.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh7058.h"
#include "src/algorithms/checksum/checksum_ecu_subaru_hitachi_sh72543r.h"
#include "src/algorithms/checksum/checksum_tcu_mitsu_mh8104_can.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_denso_sh7055.h"
#include "src/algorithms/checksum/checksum_tcu_subaru_hitachi_m32r_can.h"
#include "src/algorithms/checksum/qt_checksum.h"
#include "src/algorithms/protocol/qt_bytes.h"
#include "test_checksum_results.h"

namespace
{

void appendU32(QByteArray *data, quint32 value)
{
    data->append(char((value >> 24) & 0xff));
    data->append(char((value >> 16) & 0xff));
    data->append(char((value >> 8) & 0xff));
    data->append(char(value & 0xff));
}

void compareChangedBytes(const QByteArray& original,
                         const QByteArray& actual,
                         const QList<QPair<int, QByteArray>>& replacements)
{
    QByteArray expected = original;
    for (const auto& replacement : replacements)
        expected.replace(replacement.first, replacement.second.size(), replacement.second);
    QCOMPARE(actual, expected);
}

QByteArray densoRomWithChecksumTable(quint32 storedChecksum)
{
    QByteArray rom;
    rom.append(QByteArray(4, char(0x00)));
    appendU32(&rom, 0x00000001);
    rom.append(QByteArray(8, char(0x00)));
    appendU32(&rom, 0x00000004);
    appendU32(&rom, 0x00000008);
    appendU32(&rom, storedChecksum);
    return rom;
}

} // namespace

class TestChecksumResults : public QObject
{
    Q_OBJECT

  private slots:
    void densoSh705xDiesel_correctsSingleZeroRecord()
    {
        const QByteArray original(12, '\0');

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::view(original), 0, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Denso SH705x Checksum"));
        compareChangedBytes(original, result.romData,
                            {{0, QByteArray::fromHex("00000000000000005aa5a55a")}});
    }

    void densoSh705xDiesel_correctedRecordTriggersDisabledCompatibility()
    {
        const QByteArray original = QByteArray::fromHex("00000000000000005aa5a55a");

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::view(original), 0, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Disabled);
        QCOMPARE(result.message, QString("ROM has all checksums disabled"));
        // Matches the legacy calculate_checksum()'s `return 0;` on this path:
        // romData comes back empty rather than the original ROM bytes.
        QCOMPARE(result.romData, QByteArray());
    }

    void densoSh705xDiesel_keepsMatchingRecord()
    {
        const QByteArray original = QByteArray::fromHex("00000004000000085aa5a552");

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum_result(bytes::view(original), 0, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(result.romData, original);
    }

    void densoSh7xxx_returnsUnchangedForMatchingChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x5aa5a559);

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(rom), 16, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
        QVERIFY(!result.changed());
    }

    void densoSh7xxx_returnsCorrectedDataForMismatchedChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x00000000);

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(rom), 16, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QByteArray expected = rom;
        expected.replace(24, 4, QByteArray::fromHex("5aa5a559"));
        QCOMPARE(result.romData, expected);
        QVERIFY(result.ok());
        QVERIFY(result.changed());

        const QtChecksumResult unchangedResult = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(result.romData), 16, 12));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void densoSh7xxx_returnsDisabledWithoutClearingRomData()
    {
        QByteArray rom(16, char(0x00));
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x5aa5a55a);

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(rom), 16, 12));

        QCOMPARE(result.status, ChecksumResult::Status::Disabled);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
    }

    void densoSh7xxx_rejectsChecksumAreaOutsideRom()
    {
        const QByteArray rom(16, char(0x00));

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(rom), 16, 12));

        QCOMPARE(result.status, ChecksumResult::Status::InvalidSize);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }

    void densoSh7xxx_rejectsNonTableAlignedChecksumArea()
    {
        const QByteArray rom(16, char(0x00));

        const QtChecksumResult result = toQt(ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(bytes::view(rom), 0, 10));

        QCOMPARE(result.status, ChecksumResult::Status::ParseError);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }

    void hitachiM32rCan_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        const QtChecksumResult result = toQt(ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi M32R CAN ECU Checksum"));
        compareChangedBytes(original, result.romData, {{0x7fffa, QByteArray::fromHex("5aa5")}});

        // Checksum 6's mismatch handling never writes the fix back to romData
        // (dead code in the algorithm, unchanged by this task), so checksum_ok
        // stays false forever and a second pass keeps reporting Corrected even
        // though the bytes have stopped changing. Matches the pre-existing
        // legacy calculate_checksum() behavior, which only asserted byte
        // stability across a second pass, not a status transition.
        const QtChecksumResult secondPass = toQt(ChecksumEcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(secondPass.status, ChecksumResult::Status::Corrected);
        QCOMPARE(secondPass.romData, result.romData);
    }

    void hitachiM32rKline_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        const QtChecksumResult result = toQt(ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi M32R K-Line ECU Checksum"));
        compareChangedBytes(original, result.romData, {{0x7fffa, QByteArray::fromHex("5aa5")}});

        const QtChecksumResult secondPass = toQt(ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(secondPass.status, ChecksumResult::Status::Corrected);
        QCOMPARE(secondPass.message, QString("Subaru Hitachi M32R K-Line ECU Checksum"));
        compareChangedBytes(result.romData, secondPass.romData, {{0x8100, QByteArray::fromHex("ffff")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumEcuSubaruHitachiM32rKline::calculate_checksum_result(bytes::view(secondPass.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, secondPass.romData);
    }

    void hitachiSh7058_balancesZeroRom()
    {
        const QByteArray original(0x100000, '\0');

        const QtChecksumResult result = toQt(ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi SH7058 CAN ECU Checksum"));
        compareChangedBytes(original, result.romData,
                            {{0xffff0, QByteArray::fromHex("5aa5a55a")},
                             {0xffff4, QByteArray::fromHex("5aa5a55a")},
                             {0xffff8, QByteArray::fromHex("5aa5a55a")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumEcuSubaruHitachiSH7058::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void hitachiSh72543r_balancesZeroRom()
    {
        const QByteArray original(0x200000, '\0');

        const QtChecksumResult result = toQt(ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi SH72543r ECU Checksum"));
        compareChangedBytes(original, result.romData, {{0x1ffffe, QByteArray::fromHex("5aa5")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumEcuSubaruHitachiSh72543r::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void mitsuMh8104Tcu_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        const QtChecksumResult result = toQt(ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi M32R K-Line/CAN ECU Checksum"));
        compareChangedBytes(original, result.romData, {{0x81fc, QByteArray::fromHex("5aa55aa5")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumTcuMitsuMH8104Can::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void densoSh7055Tcu_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        const QtChecksumResult result = toQt(ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Denso SH7055 TCU Checksum"));
        compareChangedBytes(original, result.romData, {{0x7fff4, QByteArray::fromHex("5aa5")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumTcuSubaruDensoSH7055::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void hitachiM32rCanTcu_balancesZeroRom()
    {
        const QByteArray original(0x10000, '\0');

        const QtChecksumResult result = toQt(ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::view(original)));
        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QCOMPARE(result.message, QString("Subaru Hitachi M32R K-Line/CAN ECU Checksum"));
        compareChangedBytes(original, result.romData,
                            {{0x8000, QByteArray::fromHex("a55a5aa6")},
                             {0x8004, QByteArray::fromHex("a55a5aa6")},
                             {0x8020, QByteArray::fromHex("5aa5a55a")}});

        const QtChecksumResult unchangedResult = toQt(ChecksumTcuSubaruHitachiM32rCan::calculate_checksum_result(bytes::view(result.romData)));
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }
};

int run_test_checksum_results(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestChecksumResults test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_checksum_results.moc"
