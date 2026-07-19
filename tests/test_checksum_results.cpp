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
#include "expected_message_box.h"
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

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Denso SH705x Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum(original, 0, 12);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual,
                            {{0, QByteArray::fromHex("00000000000000005aa5a55a")}});
    }

    void densoSh705xDiesel_correctedRecordTriggersDisabledCompatibility()
    {
        const QByteArray original = QByteArray::fromHex("00000000000000005aa5a55a");

        ExpectedMessageBoxCloser closer;
        closer.arm("32-bit checksum", "ROM has all checksums disabled");
        const QByteArray actual = ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum(original, 0, 12);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        QCOMPARE(actual, QByteArray());
    }

    void densoSh705xDiesel_keepsMatchingRecord()
    {
        const QByteArray original = QByteArray::fromHex("00000004000000085aa5a552");

        const QByteArray actual = ChecksumEcuSubaruDensoSH705xDiesel::calculate_checksum(original, 0, 12);

        QCOMPARE(actual, original);
    }

    void densoSh7xxx_returnsUnchangedForMatchingChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x5aa5a559);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
        QVERIFY(!result.changed());
    }

    void densoSh7xxx_returnsCorrectedDataForMismatchedChecksum()
    {
        const QByteArray rom = densoRomWithChecksumTable(0x00000000);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Corrected);
        QByteArray expected = rom;
        expected.replace(24, 4, QByteArray::fromHex("5aa5a559"));
        QCOMPARE(result.romData, expected);
        QVERIFY(result.ok());
        QVERIFY(result.changed());

        const ChecksumResult unchangedResult =
            ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(result.romData, 16, 12);
        QCOMPARE(unchangedResult.status, ChecksumResult::Status::Unchanged);
        QCOMPARE(unchangedResult.romData, result.romData);
    }

    void densoSh7xxx_returnsDisabledWithoutClearingRomData()
    {
        QByteArray rom(16, char(0x00));
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x00000000);
        appendU32(&rom, 0x5aa5a55a);

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::Disabled);
        QCOMPARE(result.romData, rom);
        QVERIFY(result.ok());
    }

    void densoSh7xxx_rejectsChecksumAreaOutsideRom()
    {
        const QByteArray rom(16, char(0x00));

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 16, 12);

        QCOMPARE(result.status, ChecksumResult::Status::InvalidSize);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }

    void densoSh7xxx_rejectsNonTableAlignedChecksumArea()
    {
        const QByteArray rom(16, char(0x00));

        const ChecksumResult result = ChecksumEcuSubaruDensoSH7xxx::calculate_checksum_result(rom, 0, 10);

        QCOMPARE(result.status, ChecksumResult::Status::ParseError);
        QCOMPARE(result.romData, rom);
        QVERIFY(!result.ok());
    }

    void hitachiM32rCan_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi M32R CAN ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumEcuSubaruHitachiM32rCan::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual, {{0x7fffa, QByteArray::fromHex("5aa5")}});

        closer.arm("Subaru Hitachi M32R CAN ECU Checksum", "Checksums corrected");
        const QByteArray unchanged = ChecksumEcuSubaruHitachiM32rCan::calculate_checksum(actual);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        QCOMPARE(unchanged, actual);
    }

    void hitachiM32rKline_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi M32R K-Line ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumEcuSubaruHitachiM32rKline::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual, {{0x7fffa, QByteArray::fromHex("5aa5")}});

        closer.arm("Subaru Hitachi M32R K-Line ECU Checksum", "Checksums corrected");
        const QByteArray secondPass = ChecksumEcuSubaruHitachiM32rKline::calculate_checksum(actual);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(actual, secondPass, {{0x8100, QByteArray::fromHex("ffff")}});

        const QByteArray unchanged = ChecksumEcuSubaruHitachiM32rKline::calculate_checksum(secondPass);
        QCOMPARE(unchanged, secondPass);
    }

    void hitachiSh7058_balancesZeroRom()
    {
        const QByteArray original(0x100000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi SH7058 CAN ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumEcuSubaruHitachiSH7058::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual,
                            {{0xffff0, QByteArray::fromHex("5aa5a55a")},
                             {0xffff4, QByteArray::fromHex("5aa5a55a")},
                             {0xffff8, QByteArray::fromHex("5aa5a55a")}});

        const QByteArray unchanged = ChecksumEcuSubaruHitachiSH7058::calculate_checksum(actual);
        QCOMPARE(unchanged, actual);
    }

    void hitachiSh72543r_balancesZeroRom()
    {
        const QByteArray original(0x200000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi SH72543r ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumEcuSubaruHitachiSh72543r::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual, {{0x1ffffe, QByteArray::fromHex("5aa5")}});

        const QByteArray unchanged = ChecksumEcuSubaruHitachiSh72543r::calculate_checksum(actual);
        QCOMPARE(unchanged, actual);
    }

    void mitsuMh8104Tcu_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi M32R K-Line/CAN ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumTcuMitsuMH8104Can::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual, {{0x81fc, QByteArray::fromHex("5aa55aa5")}});

        const QByteArray unchanged = ChecksumTcuMitsuMH8104Can::calculate_checksum(actual);
        QCOMPARE(unchanged, actual);
    }

    void densoSh7055Tcu_balancesZeroRom()
    {
        const QByteArray original(0x80000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Denso SH7055 TCU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumTcuSubaruDensoSH7055::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual, {{0x7fff4, QByteArray::fromHex("5aa5")}});

        const QByteArray unchanged = ChecksumTcuSubaruDensoSH7055::calculate_checksum(actual);
        QCOMPARE(unchanged, actual);
    }

    void hitachiM32rCanTcu_balancesZeroRom()
    {
        const QByteArray original(0x10000, '\0');

        ExpectedMessageBoxCloser closer;
        closer.arm("Subaru Hitachi M32R K-Line/CAN ECU Checksum", "Checksums corrected");
        const QByteArray actual = ChecksumTcuSubaruHitachiM32rCan::calculate_checksum(original);
        closer.stop();
        QVERIFY2(closer.sawExpected(), qPrintable(closer.failure()));
        compareChangedBytes(original, actual,
                            {{0x8000, QByteArray::fromHex("a55a5aa6")},
                             {0x8004, QByteArray::fromHex("a55a5aa6")},
                             {0x8020, QByteArray::fromHex("5aa5a55a")}});

        const QByteArray unchanged = ChecksumTcuSubaruHitachiM32rCan::calculate_checksum(actual);
        QCOMPARE(unchanged, actual);
    }
};

int run_test_checksum_results(int argc, char **argv)
{
    QApplication app(argc, argv);
    TestChecksumResults test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_checksum_results.moc"
